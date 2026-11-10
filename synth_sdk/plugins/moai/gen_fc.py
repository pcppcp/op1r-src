#!/usr/bin/env python3
"""Write moai_fc.S: the sixteen filter chunk loops of moai.c (filter_chunk_t, FILTER_CHUNK) in Blackfin assembly,
bit for bit the C's arithmetic (kennel: harness/moai_fc_cmp.py compares every kernel with the C on bcore).

    python3 gen_fc.py            (writes moai_fc.S next to this file)

Each variant is one list of operations in the C's order, one 32-bit ALU / MAC instruction each, exactly the intrinsics
of moai.c (same multiply mode, halves and saturation). The loads and stores of coefficients, states and samples are
then packed into the two 16-bit slots beside them: at most one P-register access, slot 3 an I-register access, at most
one store, and never a register that the packet's ALU instruction writes (or, for a load, reads). When no packet in its
window has room, a load or store gets an MNOP packet of its own. Only data registers are stored, so anomaly 05000074
(a shift-immediate beside a P-addressed store of a P register; GCC's bfin.md "storep_anomaly_05000074") cannot occur;
bcore would not show it. The freedoms over the compiled C are only these: the
registers, the packing, `x.H * x.H (IS)` for MUL16(x >> 16, x >> 16), `W[I1++] = R.H` for the `>> 16` before the
16-bit store, and loading only the low half of the input sample (only `.L` is read).

Registers in the loop: R4..R7 the four ladder states; R3 the AC coupling's dc (transistor variants: the stages' t[]
and dc goes to the frame); R0..R2 work. I0 / I1 read and write the chunk, I2 / I3 read and write lad->t (the last of
four post-modifies by -12 back to t[0]). P2 = the chunk's lad_coef_t, SP the frame below.

Copyright (C) 2026 Hundekraft. Derived from moai.c, Copyright (C) 2026 moai contributors.
GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent

# memory: (base register, operand) and the stream it belongs to (accesses of one stream keep their order)
MEM = {
    'GGK': ('P', '[P2]'), 'C12': ('P', '[P2 + 4]'), 'C34': ('P', '[P2 + 8]'), 'INVK': ('P', '[P2 + 12]'),
    'GXOG': ('P', '[SP + 0]'), 'MUFB': ('P', '[SP + 4]'), 'SGLA': ('P', '[SP + 8]'), 'SB': ('P', '[SP + 12]'),
    'DC': ('P', '[SP + 16]'), 'DC2': ('P', '[SP + 20]'), 'LP': ('P', '[SP + 24]'), 'YP': ('P', '[SP + 28]'),
    'GKK': ('P', '[SP + 32]'),
}
FRAME = 36


class Op:
    def __init__(self, kind, text, reads=(), writes=(), stream=None, base=None, store=False):
        self.kind, self.text, self.reads, self.writes = kind, text, set(reads), set(writes)
        self.stream, self.base, self.store = stream, base, store
        self.nopar, self.after = False, None     # nopar: a 16-bit instruction (no parallel slots); after: a label behind it


def regs(s):
    return re.findall(r'\b(R[0-7]|A0)\b', s)


def alu(text):
    """dst = expr: the destination is the first register (A0 for the MAC chain); every register on the right is read"""
    lhs, rhs = text.split('=', 1)
    w = regs(lhs)
    r = regs(rhs)
    if lhs.strip().startswith('A0') and ('+=' in text or '-=' in text):
        r.append('A0')
    if '(A0 ' in rhs:
        r.append('A0'); w.append('A0')
    return Op('alu', text, r, w)


def ld(dst, mem, half=False):
    base, operand = MEM[mem]
    return Op('mem', f'{dst} = {operand}', (), [dst.split('.')[0]], stream=mem, base=base)


def st(src, mem):
    base, operand = MEM[mem]
    return Op('mem', f'{operand} = {src}', [src.split('.')[0]], (), stream=mem, base=base, store=True)


def ld_in(dst):
    return Op('mem', f'{dst}.L = W[I0++]', (), [dst], stream='IN', base='I')


def st_out(src):
    return Op('mem', f'W[I1++] = {src}.H', [src], (), stream='OUT', base='I', store=True)


def ld_t(dst, last):
    return Op('mem', f'{dst} = [I2 ++ M2]' if last else f'{dst} = [I2++]', (), [dst], stream='TLD', base='I')


def st_t(src, last):
    return Op('mem', f'[I3 ++ M3] = {src}' if last else f'[I3++] = {src}', [src], (), stream='TST', base='I', store=True)


def clip(a, k, t):
    """sclipk(a, k): x = SHL2S(x) >> 2; q = MUL16LH(MUL16(x >> 16, x >> 16) >> 12, kk) >> 12; x - FM_HL(x, q)"""
    return [alu(f'{a} = {a} << 2 (S)'), alu(f'{a} = {a} >>> 2'), alu(f'{t} = {a}.H * {a}.H (IS)'),
            alu(f'{t} = {t} >>> 12'), alu(f'{t} = {t}.L * {k}.H (IS)'), alu(f'{t} = {t} >>> 12'),
            alu(f'{t} = {a}.H * {t}.L'), alu(f'{a} = {a} - {t} (NS)')]


def body(fb, res, sat, tr):
    o = []
    dc = 'R3'
    # drive: x = SHL2S(FM_LLP(b[n], gxog))
    o += [ld_in('R0'), ld('R1', 'GXOG'), alu('R0 = R0.L * R1.L'), alu('R0 = R0 << 2 (S)')]
    if tr:
        o.append(ld_t('R3', False))                                  # t[0]
    if fb:                                                           # x = ADDS(x, SHL2S(FM_HH(yp - dc, mufb)))
        o.append(ld('R1', 'YP'))
        if tr:
            o += [ld('R2', 'DC'), alu('R1 = R1 - R2 (NS)')]
        else:
            o.append(alu('R1 = R1 - R3 (NS)'))
        o += [ld('R2', 'MUFB'), alu('R1 = R1.H * R2.H'), alu('R1 = R1 << 2 (S)'), alu('R0 = R0 + R1 (S)')]
    if res:                                                          # u = sclipk(FM_HLP(SUBS(x, SHL3S(FM_HH(S, ggk))), invk))
        o += [ld('R1', 'C12'), ld('R2', 'C34'),
              alu('A0 = R4.H * R1.L'), alu('A0 += R5.H * R1.H'), alu('A0 += R6.H * R2.L'), alu('R2 = (A0 += R7.H * R2.H)'),
              ld('R1', 'GGK'), alu('R2 = R2.H * R1.H'), alu('R2 = R2 << 3 (S)'), alu('R0 = R0 - R2 (S)'),
              ld('R1', 'INVK'), alu('R0 = R0.H * R1.L')]
    else:
        o.append(ld('R1', 'INVK'))
    o += clip('R0', 'R1', 'R2')
    if tr:      # v = FM_HLP(SUBS(in, t[i]), ggk); y = s + v; s = y + v; t[i] = y = stclip(y)   (gkk: gg | CLIP_K << 16)
        o.append(ld('R1', 'GKK'))
        for i, s in enumerate(('R4', 'R5', 'R6', 'R7')):
            o += [alu('R2 = R0 - R3 (S)'), alu('R0 = R2.H * R1.L'), alu(f'R2 = {s} + R0 (NS)'), alu(f'{s} = R2 + R0 (NS)')]
            if i < 3:
                o.append(ld_t('R3', i == 2))
            o += [alu('R0 = R2 << 2 (S)'), alu('R0 = R0.H * R0.H'), alu('R0 = R0.H * R1.H'), alu('R0 = R2.H * R0.H'),
                  alu('R0 = R0 << 2 (S)'), alu('R0 = R2 - R0 (NS)'), st_t('R0', i == 3)]
        # AC coupling: y = yp - dc; dc += y >> 10  (dc in the frame; R1 = y; 14 Hz at 88.2 kHz)
        o += [st('R2', 'YP'), ld('R3', 'DC'), alu('R1 = R2 - R3 (NS)'), alu('R2 = R1 >>> 10'), alu('R3 = R3 + R2 (NS)'),
              st('R3', 'DC'), ld('R2', 'MUFB'), alu('R0 = R1.H * R2.L'), alu('R0 = R0 << 2 (S)')]
    else:       # v = FM_HLP(SUBS(in, s), ggk); y = s + v; s = y + v
        o.append(ld('R1', 'GGK'))
        for i, s in enumerate(('R4', 'R5', 'R6', 'R7')):
            o += [alu(f'R0 = {"R0" if i == 0 else "R2"} - {s} (S)'), alu('R0 = R0.H * R1.L'),
                  alu(f'R2 = {s} + R0 (NS)'), alu(f'{s} = R2 + R0 (NS)')]
        o += [st('R2', 'YP'), alu(f'R0 = R2 - {dc} (NS)'), alu('R2 = R0 >>> 10'), alu(f'{dc} = {dc} + R2 (NS)'),
              ld('R1', 'MUFB'), alu('R0 = R0.H * R1.L'), alu('R0 = R0 << 2 (S)')]
    if sat:     # sclipk(ADDS(SHL3S(FM_HLP(y, sgla)), sb)); y -= dc2; dc2 += y >> 10; lp += FM_HH(y - lp, sgla); y = lp
        o += [ld('R1', 'SGLA'), alu('R0 = R0.H * R1.L'), alu('R0 = R0 << 3 (S)'), ld('R2', 'SB'), alu('R0 = R0 + R2 (S)'),
              ld('R1', 'INVK')]
        o += clip('R0', 'R1', 'R2')
        o += [ld('R2', 'DC2'), alu('R0 = R0 - R2 (NS)'), alu('R1 = R0 >>> 10'), alu('R2 = R2 + R1 (NS)'), st('R2', 'DC2'),
              ld('R1', 'LP'), alu('R0 = R0 - R1 (NS)'), ld('R2', 'SGLA'), alu('R0 = R0.H * R2.H'), alu('R0 = R1 + R0 (NS)'),
              st('R0', 'LP')]
    else:
        o.append(ld('R1', 'INVK'))
        o += clip('R0', 'R1', 'R2')
    # b[n] = SHL3S(FM_HH(y, gxog)) >> 16
    o += [ld('R1', 'GXOG'), alu('R0 = R0.H * R1.H'), alu('R0 = R0 << 3 (S)'), st_out('R0')]
    return o


def schedule(ops):
    """-> packets [[alu or None, mem...]]: ALU instructions in order, each memory access in a packet of its window"""
    alus = [op for op in ops if op.kind == 'alu']
    packets = [[a] for a in alus]
    pos = {}                                     # op -> index into ops of its ALU anchor
    seq = []                                     # ops in order with their ALU index at that point
    n = 0
    for op in ops:
        if op.kind == 'alu':
            n += 1
        seq.append((op, n))                      # a memory op sits between ALU n-1 and ALU n (0-based: before alus[n])

    def find(p):                                 # packet index of an ALU op
        for i, pk in enumerate(packets):
            if pk[0] is p:
                return i
        raise KeyError

    def where(m):
        return next(i for i, pk in enumerate(packets) if any(x is m for x in pk[1:]))

    last_in_stream = {}
    placed = []
    for idx, (op, n) in enumerate(seq):
        if op.kind == 'alu':
            continue
        reg = next(iter(op.writes or op.reads))
        if not op.store:
            # window: after the last ALU before this point that reads or writes reg, before the next ALU reading it
            prev = max((j for j in range(n) if reg in alus[j].reads | alus[j].writes), default=None)
            cons = next(j for j in range(n, len(alus)) if reg in alus[j].reads)
            lo = find(alus[prev]) + 1 if prev is not None else 0
            hi = find(alus[cons]) - 1
        else:
            prod = max((j for j in range(n) if reg in alus[j].writes), default=None)
            nxt = next((j for j in range(n, len(alus)) if reg in alus[j].writes), None)
            lo = find(alus[prod]) + 1 if prod is not None else 0
            hi = find(alus[nxt]) - 1 if nxt is not None else len(packets) - 1
        if op.stream in last_in_stream:
            lo = max(lo, last_in_stream[op.stream] + 1)
        for m in placed:                         # an earlier load or store of the same register stays earlier
            if (m.reads | m.writes) & (op.reads | op.writes):
                lo = max(lo, where(m) + 1)
        cands = range(hi, lo - 1, -1) if not op.store else range(lo, hi + 1)

        def fits(p):
            pk = packets[p]
            a, mems = pk[0], pk[1:]
            if len(mems) >= 2 or (a is not None and a.nopar):
                return False
            if a is not None and (reg in a.writes or (not op.store and reg in a.reads)):
                return False
            if op.base == 'P' and any(m.base == 'P' for m in mems):
                return False
            if op.store and any(m.store for m in mems):
                return False
            if any(m.writes & (op.writes | op.reads) or op.writes & m.reads for m in mems):
                return False
            if any(m.stream == op.stream for m in mems):
                return False
            return True
        p = next((p for p in cands if fits(p)), None)
        if p is None:                            # an MNOP packet of its own, at the window's edge
            if lo > hi + 1:
                raise SystemExit(f'no packet for {op.text}')
            p = (hi + 1) if not op.store else lo
            packets.insert(p, [None])
            for s in last_in_stream:
                if last_in_stream[s] >= p:
                    last_in_stream[s] += 1
        packets[p].append(op)
        last_in_stream[op.stream] = p
        placed.append(op)
    return packets


def emit_packet(pk):
    a, mems = pk[0], pk[1:]
    parts = [a.text if a else 'MNOP'] + [m.text for m in sorted(mems, key=lambda m: m.base != 'P')]
    if len(parts) == 1:
        return f'\t{parts[0]};'
    # slot 2 may be P or I, slot 3 only I: P first
    if len(parts) == 2 and a is None and mems[0].base == 'I':
        return f'\tMNOP || {parts[1]};'
    return '\t' + ' || '.join(parts) + ';'


def variant(name, fb, res, sat, tr):
    ops = body(fb, res, sat, tr)
    packets = schedule(ops)
    sect = '.section .text.mm_tr, "ax"' if tr else '.text'
    out = [sect, '\t.balign 32', f'\t.global _{name}', f'\t.type _{name}, STT_FUNC',
           f'/* {name}: fb {fb}, res {res}, sat {sat}, tr {tr}; {len(packets)} packets a sample */', f'_{name}:',
           '\t[--SP] = (R7:4);', f'\tSP += -{FRAME};',
           '\tI0 = R0;', '\tI1 = R0;', '\tP0 = R1;', '\tP2 = R2;', '\tR3 = 0;',
           '\tL0 = R3;', '\tL1 = R3;', '\tL2 = R3;', '\tL3 = R3;',
           f'\tR0 = [SP + {FRAME + 16 + 12}];', '\t[SP + 0] = R0;',          # gxog
           f'\tP1 = [SP + {FRAME + 16 + 20}];',                                # st
           f'\tR1 = [SP + {FRAME + 16 + 16}];', '\tR0 = [P1 + 12];', '\tR0 = PACK(R1.L, R0.L);', '\t[SP + 4] = R0;']   # mu | fb << 16
    if sat:
        out += ['\tR0 = [P1];', '\tR1 = [P1 + 8];', '\tR0 = PACK(R1.L, R0.L);', '\t[SP + 8] = R0;',   # g | lp << 16
                '\tR0 = [P1 + 4];', '\t[SP + 12] = R0;']                                    # b
    out += [f'\tP1 = [SP + {FRAME + 16 + 28}];', '\tR3 = [P1];']                           # pdc: dc
    if tr:
        out.append('\t[SP + 16] = R3;')
    if sat:
        out += ['\tR0 = [P1 + 4];', '\t[SP + 20] = R0;', '\tR0 = [P1 + 8];', '\t[SP + 24] = R0;']
    out += [f'\tP1 = [SP + {FRAME + 16 + 24}];', '\tR0 = [P1];', '\t[SP + 28] = R0;']      # yp
    if tr:
        out += ['\tR0 = [P2];', '\tR1 = [P2 + 12];', '\tR0 = PACK(R1.H, R0.L);', '\t[SP + 32] = R0;',  # gg | CLIP_K << 16
                '\tR0 = P0;', '\tR0 += 16;', '\tI2 = R0;', '\tI3 = R0;', '\tR0 = -12;', '\tM2 = R0;', '\tM3 = R0;']
    out += ['\tR4 = [P0];', '\tR5 = [P0 + 4];', '\tR6 = [P0 + 8];', '\tR7 = [P0 + 12];', '\tP1 = 32;',
            f'\tLSETUP (.L{name}_b, .L{name}_e) LC0 = P1;']
    for i, pk in enumerate(packets):
        label = f'.L{name}_b:' if i == 0 else (f'.L{name}_e:' if i == len(packets) - 1 else None)
        if label:
            out.append(label)
        out.append(emit_packet(pk))
        if pk[0] is not None and pk[0].after:
            out.append(pk[0].after + ':')
    out += ['\t[P0] = R4;', '\t[P0 + 4] = R5;', '\t[P0 + 8] = R6;', '\t[P0 + 12] = R7;',
            f'\tP1 = [SP + {FRAME + 16 + 28}];']
    if tr:
        out += ['\tR3 = [SP + 16];']
    out.append('\t[P1] = R3;')
    if sat:
        out += ['\tR0 = [SP + 20];', '\t[P1 + 4] = R0;', '\tR0 = [SP + 24];', '\t[P1 + 8] = R0;']
    out += [f'\tP1 = [SP + {FRAME + 16 + 24}];', '\tR0 = [SP + 28];', '\t[P1] = R0;',
            f'\tSP += {FRAME};', '\t(R7:4) = [SP++];', '\tRTS;', f'\t.size _{name}, .-_{name}', '']
    return out, len(packets)


VARIANTS = [('fc_plain', 0, 0, 0, 0), ('fc_sat', 0, 0, 1, 0), ('fc_res', 0, 1, 0, 0), ('fc_res_sat', 0, 1, 1, 0),
            ('fc_fb', 1, 0, 0, 0), ('fc_fb_sat', 1, 0, 1, 0), ('fc_fb_res', 1, 1, 0, 0), ('fc_fb_res_sat', 1, 1, 1, 0),
            ('fc_tr', 0, 0, 0, 1), ('fc_sat_tr', 0, 0, 1, 1), ('fc_res_tr', 0, 1, 0, 1), ('fc_res_sat_tr', 0, 1, 1, 1),
            ('fc_fb_tr', 1, 0, 0, 1), ('fc_fb_sat_tr', 1, 0, 1, 1), ('fc_fb_res_tr', 1, 1, 0, 1),
            ('fc_fb_res_sat_tr', 1, 1, 1, 1)]


def main():
    lines = ['/* moai_fc.S: moai.c\'s sixteen filter chunk loops (filter_chunk_t) in Blackfin assembly, bit for bit.',
             ' * Generated by gen_fc.py (edit that, not this file). Arguments as filter_chunk_t: R0 = b, R1 = lad, R2 = lc,',
             ' * [SP + 12] = gxog, [SP + 16] = fb, [SP + 20] = st, [SP + 24] = pyp, [SP + 28] = pdc.',
             ' * Copyright (C) 2026 Hundekraft. Derived from moai.c, Copyright (C) 2026 moai contributors.',
             ' * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */', '']
    counts = []
    for v in VARIANTS:
        out, n = variant(*v)
        lines += out
        counts.append((v[0], n))
    lines = lines[:5] + ['#ifdef __bfin__                 /* the host builds (golden.py, the tests) use the C */', ''] \
        + lines[5:] + ['#endif', '']
    (HERE / 'moai_fc.S').write_text('\n'.join(lines))
    for name, n in counts:
        print(f'{name:18} {n} packets a sample')


if __name__ == '__main__':
    main()
