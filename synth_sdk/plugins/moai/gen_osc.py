#!/usr/bin/env python3
"""Write moai_osc.S: moai.c's oscillator passes (osc_loop: one oscillator, osc_loop2: two of the same kind) in Blackfin
assembly for the saw, pulse, triangle and shark waves, adding or writing, bit for bit the C (kennel:
harness/moai_fc_cmp.py compares every pass with the C on bcore). Noise stays in C.

    python3 gen_osc.py           (writes moai_osc.S next to this file)

The packing is gen_fc.py's. Per sample and oscillator: p += inc; inc += dinc. Per saw (at p, or at p + w for a pulse's
second edge): s = (p ^ 2^31) >> 17 as (p + 2^31) >>> 17, and the polyBLEP test p + inc < 2 inc (unsigned) as a compare
and a branch to an edge routine out of the loop; the routine (rare: within one sample of an edge) saves the two
registers it uses, adds or subtracts the C's MUL16(u, u) >> 14 to s, restores them and jumps back. The triangle is
2 |s| - 16384 as (|s| - 8192) << 1, from s before the edge correction (tri_of recomputes it from p). The weighted
sum runs on A0 in integer mode: |weights| <= 16383 (level <= 16383, wave weight <= 32767) and every MUL16 operand is
its low half, so the at most six products stay inside 32 bits and the accumulator gives the C's wrapping sum; then
>> 15 and the 16-bit store (adding: only the low half of buf[n] matters, so it is loaded into .L alone).

Arguments: R0 = buf, R1 = osc_args_t (moai.c): 2^31 and -8192 at 0 and 4; dinc, w, a1, a2, a3 of the first oscillator
at 8..24 and of the second at 28..44 (the loop's loads, within the short form's +60); p, inc, r at 48..56 and 60..68.
The phases go back into the struct.

Copyright (C) 2026 Hundekraft. Derived from moai.c, Copyright (C) 2026 moai contributors.
GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
from pathlib import Path

import gen_fc as G

HERE = Path(__file__).resolve().parent
# osc_args_t: what the loop reads every sample within the short form's reach (P + 0..60), the rest after it
C80, M8192 = 0, 4
OFS = [{'dinc': 8, 'w': 12, 'a1': 16, 'a2': 20, 'a3': 24, 'p': 48, 'inc': 52, 'r': 56},
       {'dinc': 28, 'w': 32, 'a1': 36, 'a2': 40, 'a3': 44, 'p': 60, 'inc': 64, 'r': 68}]


def ldk(dst, osc, field):
    return G.Op('mem', f'{dst} = [P2 + {OFS[osc][field]}]', (), [dst], stream=f'{osc}{field}', base='P')


def ldc(dst, ofs):
    return G.Op('mem', f'{dst} = [P2 + {ofs}]', (), [dst], stream=f'c{ofs}', base='P')


def special(text, reads, writes, after=None):
    op = G.Op('alu', text, reads, writes)
    op.nopar, op.after = True, after
    return op


class Pass:
    def __init__(self, name, kind, pair, add):
        self.name, self.kind, self.pair, self.add = name, kind, pair, add
        self.edges = []                          # (label, return label, x, inc, t, osc)
        self.first = True                        # the next product starts A0

    def mac(self, t, wreg, osc, field, sign, last):
        op = '-=' if sign < 0 else '+='
        o = [ldk(wreg, osc, field)]
        if last and self.first:
            o.append(G.alu(f'R0 = {t}.L * {wreg}.L (IS)') if sign > 0 else None)
        elif last:
            o.append(G.alu(f'R0 = (A0 {op} {t}.L * {wreg}.L) (IS)'))
        elif self.first:
            o.append(G.alu(f'A0 = {t}.L * {wreg}.L (IS)'))
        else:
            o.append(G.alu(f'A0 {op} {t}.L * {wreg}.L (IS)'))
        self.first = False
        return [x for x in o if x is not None]

    def saw(self, x, inc, t, tmp, w2, osc):
        """t = the band-limited saw at phase register x (inc: the oscillator's, w2 = 2 inc)"""
        k = len(self.edges)
        lab, ret = f'.L{self.name}_e{k}', f'.L{self.name}_r{k}'
        self.edges.append((lab, ret, x, inc, t, osc))
        return [ldc(tmp, C80), G.alu(f'{t} = {x} + {tmp} (NS)'), G.alu(f'{t} = {t} >>> 17'),
                G.alu(f'{tmp} = {x} + {inc} (NS)'), special(f'CC = {tmp} < {w2} (IU)', [tmp, w2], []),
                special(f'IF CC JUMP {lab}', [x, inc, t], [t], after=ret)]

    def tri(self, t, q, tmp):
        """q = tri_of(p) from t = (p ^ 2^31) >> 17 (before any edge correction)"""
        return [G.alu(f'{q} = ABS {t}'), ldc(tmp, M8192), special(f'{q} = ({q} + {tmp}) << 1', [q, tmp], [q])]   # a 16-bit instruction: no parallel slots

    def osc(self, osc, p, inc, last):
        """one oscillator's sample: its phase step and its products on A0"""
        o = [ldk('R1', osc, 'dinc'), G.alu(f'{p} = {p} + {inc} (NS)'), G.alu(f'{inc} = {inc} + R1 (NS)')]
        kind = self.kind
        if kind in ('tri', 'shark'):
            o += [ldc('R1', C80), G.alu(f'R3 = {p} + R1 (NS)'), G.alu('R3 = R3 >>> 17')]
            o += self.tri('R3', 'R2', 'R1')                               # the triangle, from the uncorrected s
            o += self.mac('R2', 'R0', osc, 'a3', 1, last and kind == 'tri')
            if kind == 'tri':
                return o
        o.append(G.alu(f'R2 = {inc} + {inc} (NS)'))                     # 2 inc
        if kind == 'shark':
            # the saw's s is R3 already: only the edge test and its correction
            k = len(self.edges)
            lab, ret = f'.L{self.name}_e{k}', f'.L{self.name}_r{k}'
            self.edges.append((lab, ret, p, inc, 'R3', osc))
            o += [G.alu(f'R1 = {p} + {inc} (NS)'), special('CC = R1 < R2 (IU)', ['R1', 'R2'], []),
                  special(f'IF CC JUMP {lab}', [p, inc, 'R3'], ['R3'], after=ret)]
            o += self.mac('R3', 'R0', osc, 'a1', 1, last)
            return o
        o += self.saw(p, inc, 'R3', 'R1', 'R2', osc)
        if kind == 'saw':
            o += self.mac('R3', 'R0', osc, 'a1', 1, last)
            return o
        # pulse: a1 saw(p) - a2 saw(p + w)
        o += self.mac('R3', 'R0', osc, 'a1', 1, False)
        o += [ldk('R1', osc, 'w'), G.alu(f'R0 = {p} + R1 (NS)')]
        o += self.saw('R0', inc, 'R3', 'R1', 'R2', osc)
        o += self.mac('R3', 'R1', osc, 'a2', -1, last)
        return o

    def body(self):
        o = []
        if self.pair:
            o += self.osc(0, 'R4', 'R5', False)
            o += self.osc(1, 'R6', 'R7', True)
        else:
            o += self.osc(0, 'R4', 'R5', True)
        o.append(G.alu('R0 = R0 >>> 15'))
        if self.add:
            o += [G.Op('mem', 'R1.L = W[I0++]', (), ['R1'], stream='IN', base='I'), G.alu('R0 = R0 + R1 (NS)')]
        o.append(G.Op('mem', 'W[I1++] = R0.L', ['R0'], (), stream='OUT', base='I', store=True))
        return o

    def edge(self, lab, ret, x, inc, t, osc):
        """the polyBLEP correction: after the edge (x < inc) s += u^2 >> 14, before it s -= u^2 >> 14, with
        u = 16384 - ((d >> 8) * r >> 16), d = x or 2^32 - x"""
        a, b = [r for r in ('R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7') if r not in (x, inc, t)][:2]
        n = lab + '_b'
        return [f'{lab}:', f'\t[--SP] = {a};', f'\t[--SP] = {b};', f'\t{b} = [P2 + {OFS[osc]["r"]}];',
                f'\tCC = {x} < {inc} (IU);', f'\tIF !CC JUMP {n};',
                f'\t{a} = {x} >> 8;', f'\t{a} *= {b};', f'\t{a} >>= 16;', f'\t{b} = 16384 (X);', f'\t{a} = {b} - {a};',
                f'\t{a} = {a}.L * {a}.L (IS);', f'\t{a} >>>= 14;', f'\t{t} = {t} + {a};', f'\tJUMP {lab}_p;',
                f'{n}:', f'\t{a} = -{x};', f'\t{a} >>= 8;', f'\t{a} *= {b};', f'\t{a} >>= 16;', f'\t{b} = 16384 (X);',
                f'\t{a} = {b} - {a};', f'\t{a} = {a}.L * {a}.L (IS);', f'\t{a} >>>= 14;', f'\t{t} = {t} - {a};',
                f'{lab}_p:', f'\t{b} = [SP++];', f'\t{a} = [SP++];', f'\tJUMP {ret};']

    def emit(self):
        packets = G.schedule(self.body())
        name = self.name
        out = ['.text', '\t.balign 32', f'\t.global _{name}', f'\t.type _{name}, STT_FUNC',
               f'/* {name}: {self.kind}, {"two oscillators" if self.pair else "one oscillator"}, '
               f'{"adding" if self.add else "writing"}; {len(packets)} packets a sample */', f'_{name}:',
               '\t[--SP] = (R7:4);', '\tI0 = R0;', '\tI1 = R0;', '\tP2 = R1;', '\tR3 = 0;', '\tL0 = R3;', '\tL1 = R3;',
               '\tR4 = [P2 + 48];', '\tR5 = [P2 + 52];']
        if self.pair:
            out += ['\tR6 = [P2 + 60];', '\tR7 = [P2 + 64];']
        out += ['\tP1 = 128;', f'\tLSETUP (.L{name}_b, .L{name}_x) LC0 = P1;']
        for i, pk in enumerate(packets):
            if i == 0:
                out.append(f'.L{name}_b:')
            if i == len(packets) - 1:
                out.append(f'.L{name}_x:')
            out.append(G.emit_packet(pk))
            if pk[0] is not None and pk[0].after:
                out.append(pk[0].after + ':')
        out += ['\t[P2 + 48] = R4;']
        if self.pair:
            out.append('\t[P2 + 60] = R6;')
        out += ['\t(R7:4) = [SP++];', '\tRTS;']
        for e in self.edges:
            out += self.edge(*e)
        out += [f'\t.size _{name}, .-_{name}', '']
        return out, len(packets)


def variants():
    for kind in ('saw', 'pulse', 'tri', 'shark'):
        for pair in (0, 1):
            for add in (0, 1):
                yield f'os{"2" if pair else "1"}_{kind}_{"add" if add else "set"}', kind, pair, add


def main():
    lines = ['/* moai_osc.S: moai.c\'s oscillator passes (osc_loop, osc_loop2) in Blackfin assembly, bit for bit.',
             ' * Generated by gen_osc.py (edit that, not this file). R0 = buf, R1 = osc_args_t.',
             ' * Copyright (C) 2026 Hundekraft. Derived from moai.c, Copyright (C) 2026 moai contributors.',
             ' * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */', '']
    for v in variants():
        out, n = Pass(*v).emit()
        lines += out
        print(f'{v[0]:16} {n} packets a sample')
    lines = lines[:5] + ['#ifdef __bfin__                 /* the host builds (golden.py, the tests) use the C */', ''] \
        + lines[5:] + ['#endif', '']
    (HERE / 'moai_osc.S').write_text('\n'.join(lines))


if __name__ == '__main__':
    main()
