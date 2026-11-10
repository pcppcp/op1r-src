#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""Generate rv_quad_presets.h: QUAD's 90 preset rows, the reverb ring layout, the sine table and the test metadata.

Presets are written in musical units here (ms, Hz, dB, seconds of RT60) and converted to the integer rows that
rv_quad.c reads (no trig, exp or division in C). EQ: RBJ Audio EQ Cookbook biquads at QD_RATE, Q13, checked for
headroom (sum |c| < 7.3) and pole radius after quantization. Reverb: J. Dattorro, "Effect Design Part 1" (1997)
tank lengths, rescaled from 29,761 Hz to 44,100 Hz and per type. Everything runs at QD_RATE = 44,100 Hz.
Run: python3 gen_quad_presets.py  (writes rv_quad_presets.h next to this file)
"""
import cmath, math, os, re

FS = 44100.0
N = 64
HDR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rv_quad.h')
_H = open(HDR).read()


def hdef(name):              # a ring length from rv_quad.h (the generator checks every preset fits)
    return int(re.search(r'#define %s\s+(\d+)' % name, _H).group(1))


DLY_LEN, PRE_LEN, PIT_LEN, RV_LEN = hdef('QD_DLY_LEN'), hdef('QD_PRE_LEN'), hdef('QD_PIT_LEN'), hdef('QD_RV_LEN')
NEED = {}                    # the smallest ring lengths the presets need, printed by main()


def need(k, v):
    NEED[k] = max(NEED.get(k, 0), v)
P_NONE, P_CHORUS, P_FLANGE, P_DETUNE, P_PHASER, P_LESLIE = range(6)
D_NONE, D_MONO, D_STEREO, D_PING = range(4)
R_NONE, R_PLATE, R_ROOM, R_CHAMBER, R_HALL, R_REVERSE = range(6)

# ---- reverb rings (the tank at QD_RATE, TN = N samples a call): 4 input allpasses, modA, A1, A2 (allpass), A2 delay,
# modB, B1, B2 (allpass), B2 delay; each element its own ring in the pool
FT = FS
TN = N
REV_MAX_MS = 610             # the reverse line: the longest cluster (600 ms) plus a block
DAT = [142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163]
K = FT / 29761.0
TYPE_SCALE = {R_PLATE: (1.0, 0.80), R_ROOM: (0.6, 0.40), R_CHAMBER: (0.8, 0.60), R_HALL: (1.0, 0.95),
              R_REVERSE: (1.0, 0.0)}
EXC = 28                     # modulated allpass headroom, tank samples (mod <= 12 at 22,050 Hz = 24 here)
MIN_SCALE = 0.25             # preset size 0.5 x SIZE 50 %


def layout(t):
    """-> region starts, design delays, ring lengths (tank samples). The input allpasses' rings are exactly their
    delay (read and write the same slot); the tank lines' rings hold the longest delay + 2 (+ EXC when modulated)."""
    if t == R_NONE:
        return [0] * 12, [0] * 12, [0] * 12
    ds, ts = TYPE_SCALE[t]
    lens = [round(DAT[i] * K * ds) for i in range(4)]
    rings = list(lens)
    if t == R_REVERSE:
        lens += [int(REV_MAX_MS * FT / 1000)] + [0] * 7
        rings += [lens[4] + 2] + [0] * 7
    else:
        lens += [round(DAT[i] * K * ts) for i in range(4, 12)]
        rings += [l + 2 + (EXC if i in (4, 8) else 0) for i, l in zip(range(4, 12), lens[4:])]
        for i in range(4, 12):   # block-major processing needs every tank delay > TN + 1 at the smallest size
            assert int(lens[i] * MIN_SCALE) - (24 if i in (4, 8) else 0) >= TN + 2, (t, i, lens[i])
    assert all(l >= TN for l in lens[:4]), (t, lens[:4])
    w, at = [], 0
    for r in rings:
        w.append(at)
        at += r
    need('QD_RV_LEN', at)
    return w, lens, rings


def onepole_t(hz):           # one-pole coefficient at the tank rate
    return min(32767, int(round((1 - math.exp(-2 * math.pi * hz / FT)) * 32768)))


LAY = [layout(t) for t in range(6)]


def q15(x):
    v = int(round(x * 32768))
    return max(-32768, min(32767, v))


def onepole(hz):             # one-pole lowpass coefficient: y += k (x - y)
    return min(32767, q15(1 - math.exp(-2 * math.pi * hz / FS)))


def inc(hz):                 # LFO phase increment per sample
    return int(round(hz / FS * 2 ** 32)) & 0xffffffff


def smp(ms):
    return int(round(ms * FS / 1000))

# ---- RBJ biquads


def rbj(kind, f, g, q=0.707):
    A = 10 ** (g / 40)
    w = 2 * math.pi * f / FS
    c, s = math.cos(w), math.sin(w)
    if kind == 'peak':
        al = s / (2 * q)
        b = [1 + al * A, -2 * c, 1 - al * A]
        a = [1 + al / A, -2 * c, 1 - al / A]
    else:
        al = s / 2 * math.sqrt(2)      # shelf slope S = 1
        sa = 2 * math.sqrt(A) * al
        if kind == 'low':
            b = [A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa)]
            a = [(A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa]
        else:
            b = [A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa)]
            a = [(A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa]
    co = [b[0] / a[0], b[1] / a[0], b[2] / a[0], a[1] / a[0], a[2] / a[0]]
    qc = [int(round(x * 8192)) for x in co]
    assert all(-32768 <= x <= 32767 for x in qc), (kind, f, g, co)
    assert sum(abs(x) for x in qc) < 7.3 * 8192, (kind, f, g, co)
    a1, a2 = qc[3] / 8192, qc[4] / 8192
    for r in ((-a1 + cmath.sqrt(a1 * a1 - 4 * a2)) / 2, (-a1 - cmath.sqrt(a1 * a1 - 4 * a2)) / 2):
        assert abs(r) < 0.9995, (kind, f, g, abs(r))
    return qc


def eq3(ls=(120, 0), pk=(1000, 0, 0.9), hs=(6000, 0)):
    bands = []
    if ls[1]:
        bands.append(rbj('low', ls[0], ls[1]))
    if pk[1]:
        bands.append(rbj('peak', pk[0], pk[1], pk[2]))
    if hs[1]:
        bands.append(rbj('high', hs[0], hs[1]))
    return bands


GEQ_F = [64, 250, 1000, 4000, 12000]


def geq(*g):
    return [rbj('peak', f, x, 1.1) for f, x in zip(GEQ_F, g) if x]


def eq5(ls=(100, 0), p1=(400, 0, 1), p2=(1500, 0, 1), p3=(4000, 0, 1), hs=(7000, 0)):
    b = []
    if ls[1]:
        b.append(rbj('low', ls[0], ls[1]))
    for p in (p1, p2, p3):
        if p[1]:
            b.append(rbj('peak', *p))
    if hs[1]:
        b.append(rbj('high', hs[0], hs[1]))
    return b

# ---- blocks


def chorus(rate=0.8, cen=12, dep=3, dry=0.7, wet=0.7, spread=0.25):
    need('QD_PIT_LEN', smp(cen + dep) + 2 + N)
    return dict(ptype=P_CHORUS, prate=inc(rate), pcen=int(smp(cen) * 256), pdep=int(dep * FS / 1000 * 256),
                pdry=q15(dry), pwet=q15(wet), pspread=int(spread * 65536) & 0xffff)


def flange(rate=0.25, cen=2.5, dep=2.0, fb=0.6, dry=0.6, wet=0.6, spread=0.5):
    d = chorus(rate, cen, dep, dry, wet, spread)
    d.update(ptype=P_FLANGE, pfb=q15(fb))
    return d


def detune(cents=10, base=2.0, win=30.0, dry=0.7, wet=0.7):
    r = 2 ** (cents / 1200)
    W = smp(win)
    need('QD_PIT_LEN', smp(base) + W + 2 + N)
    up = -(r - 1) / W * 2 ** 32            # delay shrinks: pitch up
    dn = (1 - 1 / r) / W * 2 ** 32
    assert max(abs(up), abs(dn)) * W < 2 ** 31
    return dict(ptype=P_DETUNE, prate=int(round(up)) & 0xffffffff, prate2=int(round(dn)) & 0xffffffff,
                pcen=smp(base) * 256, pdep=W, pdry=q15(dry), pwet=q15(wet))


def apc(f):
    t = math.tan(math.pi * f / FS)
    return q15((t - 1) / (t + 1))


def phaser(rate=0.4, flo=250, fhi=3000, fb=0.4, dry=0.7, wet=0.7):
    return dict(ptype=P_PHASER, prate=inc(rate), pa0=apc(flo), pa1=apc(fhi), pfb=q15(fb), pdry=q15(dry), pwet=q15(wet))


def leslie(speed='slow', depth=1.0, am=0.35, dam=0.25, spread=0.25):
    h, d = (0.83, 0.70) if speed == 'slow' else (6.8, 5.9)
    return dict(ptype=P_LESLIE, prate=inc(h), prate2=inc(d), pcen=smp(1.36) * 256, pdep=int(0.408 * FS / 1000 * depth * 256),
                pa0=q15(am), pa1=q15(dam), px=onepole(800), pspread=int(spread * 65536) & 0xffff,
                pdry=q15(1.0), pwet=q15(1.0))


def dly(t, tl, tr=None, fb=0.3, fbr=None, damp=5000, lev=0.5):
    tr = tl if tr is None else tr
    fbr = fb if fbr is None else fbr
    need('QD_DLY_LEN', max(smp(tl), smp(tr)) + 2)
    return dict(dtype=t, dtl=smp(tl), dtr=smp(tr), dfb=q15(fb), dfbr=q15(fbr), ddamp=onepole(damp), dlev=q15(lev))


RDEF = {R_PLATE: dict(dif=(0.75, 0.625), ddif=(0.7, 0.5), damp=7000, bw=9000, er=0.0, ert=30),
        R_ROOM: dict(dif=(0.7, 0.6), ddif=(0.6, 0.45), damp=5000, bw=8000, er=0.55, ert=35),
        R_CHAMBER: dict(dif=(0.72, 0.6), ddif=(0.65, 0.5), damp=5500, bw=8500, er=0.4, ert=55),
        R_HALL: dict(dif=(0.7, 0.6), ddif=(0.6, 0.45), damp=4500, bw=8000, er=0.25, ert=70),
        R_REVERSE: dict(dif=(0.7, 0.6), ddif=(0, 0), damp=6000, bw=8000, er=0.0, ert=30)}


def rev(t, rt=2.0, size=1.0, pre=20, lev=0.5, mod=6, mrate=0.7, gate=None, revms=350, **kw):
    d = dict(RDEF[t])
    d.update(kw)
    w, lens, rings = LAY[t]
    if t == R_REVERSE:
        g, rtv = 0.0, revms / 1000.0
        assert revms * FT / 1000 * size < lens[4] - 4
    else:
        half = sum(lens[4:8]) * size
        g = min(0.95, max(0.2, 10 ** (-1.5 * half / (FT * rt))))   # two decay gains per half loop
        rtv = rt
    need('QD_PRE_LEN', smp(pre) + smp(d['ert']) + N)
    out = dict(rtype=t, rsize=q15(min(size, 0.99997)), rdec=q15(g), rdamp=onepole_t(d['damp']), rbw=onepole_t(d['bw']),
               rdif1=q15(d['dif'][0]), rdif2=q15(d['dif'][1]), rddif1=q15(d['ddif'][0]), rddif2=q15(d['ddif'][1]),
               rmod=int(mod * 2 * 256), rmrate=inc(mrate) >> 8, rer=q15(d['er']), rlev=q15(lev),
               rpre=smp(pre), rert=smp(d['ert']), rrev=int(round(revms * FT / 1000)), meta_rt=rtv)
    if gate:
        thr_db, hold_ms, rel_ms = gate
        blocks = max(1.0, rel_ms * FS / 1000 / N)
        out.update(gate=1, gthr=int(32767 * 10 ** (thr_db / 20)), ghold=max(1, round(hold_ms * FS / 1000 / N)),
                   grel=q15(10 ** (-3 / blocks)))
    return out


PRESETS = []


def P(name, cfg, eq=(), pitch=None, delay=None, reverb=None, direct=None):
    r = dict(name=name, cfg=cfg, eq=list(eq))
    for blk in (pitch, delay, reverb):
        if blk:
            r.update(blk)
    if direct is None:
        direct = 1.0 if r.get('ptype') == P_LESLIE else (0.8 if r.get('ptype') else 0.0)
    r['dir'] = q15(min(direct, 0.99997))
    PRESETS.append(r)


H, PL, RM, CH, RV = R_HALL, R_PLATE, R_ROOM, R_CHAMBER, R_REVERSE
# ---- 1: EQ > PITCH > DELAY > REVERB (30)
P('CHORUS HALL', 1, eq3(hs=(6000, -2)), chorus(), None, rev(H, 2.6, pre=25, lev=0.45))
P('CHORUS PLATE', 1, (), chorus(0.6, 10, 2.5), None, rev(PL, 1.8, pre=15, lev=0.45))
P('CHORUS ROOM', 1, (), chorus(1.1, 8, 2), None, rev(RM, 0.8, size=0.8, lev=0.5))
P('SLAP CHORUS ROOM', 1, eq3(pk=(1800, 2, 1)), chorus(0.9), dly(D_MONO, 95, fb=0.1, lev=0.45), rev(RM, 0.9, lev=0.4))
P('FLANGE HALL', 1, (), flange(), None, rev(H, 2.4, pre=30, lev=0.4))
P('JET FLANGE PLATE', 1, eq3(hs=(5000, 3)), flange(0.12, 2.0, 1.8, 0.85), None, rev(PL, 1.6, lev=0.35))
P('FLANGE ECHO ROOM', 1, (), flange(0.3, 3, 2.2, 0.5), dly(D_MONO, 280, fb=0.35, lev=0.4), rev(RM, 1.0, lev=0.35))
P('DETUNE HALL', 1, (), detune(9), None, rev(H, 2.8, pre=30, lev=0.45))
P('DETUNE PLATE', 1, (), detune(12, win=25), None, rev(PL, 2.0, pre=20, lev=0.4))
P('DETUNE SLAP ROOM', 1, eq3(ls=(150, -3)), detune(8), dly(D_STEREO, 70, 110, fb=0.0, lev=0.4), rev(RM, 0.7))
P('PHASER HALL', 1, (), phaser(0.35), None, rev(H, 2.5, lev=0.4))
P('PHASER ECHO PLATE', 1, (), phaser(0.5, 300, 2500, 0.5), dly(D_MONO, 330, fb=0.4, lev=0.4), rev(PL, 1.5, lev=0.35))
P('SLOW PHASER ROOM', 1, (), phaser(0.12, 200, 4000, 0.6), None, rev(RM, 1.1, size=0.9, lev=0.4))
P('DUAL ECHO HALL', 1, (), chorus(0.5, 9, 1.5, 0.8, 0.4), dly(D_STEREO, 250, 375, fb=0.35, lev=0.45), rev(H, 2.2, lev=0.4))
P('PING DELAY PLATE', 1, (), None, dly(D_PING, 300, fb=0.45, lev=0.55), rev(PL, 1.7, lev=0.4), direct=0.0)
P('PING DELAY HALL', 1, eq3(ls=(200, -4)), None, dly(D_PING, 420, fb=0.5, damp=3500, lev=0.5), rev(H, 3.0, lev=0.4))
P('SLAP ROOM', 1, (), None, dly(D_MONO, 110, fb=0.05, damp=7000, lev=0.5), rev(RM, 0.6, size=0.7, lev=0.4))
P('TAPE ECHO CHAMBER', 1, eq3(hs=(4000, -5)), chorus(2.5, 4, 0.3, 0.9, 0.3), dly(D_MONO, 330, fb=0.55, damp=2200, lev=0.5),
  rev(CH, 1.4, lev=0.3))
P('VOCAL PLATE', 1, eq3(ls=(180, -3), pk=(3000, 2, 1.2)), chorus(0.4, 14, 1.5, 0.85, 0.35), dly(D_MONO, 120, fb=0.1, lev=0.2),
  rev(PL, 2.2, pre=60, lev=0.5))
P('GATED SNARE', 1, eq3(pk=(220, 4, 1.4), hs=(5000, 3)), None, None, rev(H, 3.0, pre=5, lev=0.65, gate=(-30, 260, 60)))
P('GATED ROOM CHORUS', 1, (), chorus(0.8), None, rev(RM, 2.0, lev=0.55, gate=(-28, 180, 40)))
P('REVERSE CHORUS', 1, (), chorus(0.7, 12, 3), None, rev(RV, pre=10, revms=380, lev=0.6))
P('REVERSE ECHO', 1, (), None, dly(D_PING, 360, fb=0.35, lev=0.4), rev(RV, pre=5, revms=320, lev=0.55), direct=0.0)
P('DETUNE CHAMBER', 1, (), detune(7), dly(D_STEREO, 330, 440, fb=0.3, lev=0.35), rev(CH, 1.8, lev=0.4))
P('SPACE CHORUS HALL', 1, (), chorus(0.25, 18, 5, 0.6, 0.8), None, rev(H, 6.0, pre=40, lev=0.55, mod=10))
P('BIG DRUM ROOM', 1, eq3(ls=(90, 5), pk=(400, -3, 1), hs=(6000, 2)), None, None, rev(RM, 1.2, size=1.0, pre=2, lev=0.6))
P('LEAD DELAY HALL', 1, eq3(pk=(2000, 2, 0.8)), chorus(0.6, 7, 1, 0.85, 0.3), dly(D_STEREO, 380, 400, fb=0.45, damp=4000, lev=0.45),
  rev(H, 2.4, lev=0.35))
P('BALLAD HALL', 1, (), chorus(0.5, 11, 2.5), None, rev(H, 3.5, pre=80, lev=0.5))
P('WIDE DETUNE ECHO', 1, eq3(hs=(7000, 2)), detune(15, win=35), dly(D_PING, 250, fb=0.3, lev=0.35), rev(PL, 1.2, lev=0.3))
P('AMBIENT WASH', 1, eq3(ls=(250, -6)), phaser(0.08, 300, 3000, 0.3), dly(D_PING, 500, fb=0.55, damp=3000, lev=0.45),
  rev(H, 8.0, pre=50, lev=0.6, mod=12))
# ---- 2: LESLIE > DELAY > REVERB (12)
P('SLOW LESLIE HALL', 2, (), leslie('slow'), None, rev(H, 2.4, lev=0.35))
P('FAST LESLIE HALL', 2, (), leslie('fast'), None, rev(H, 2.4, lev=0.35))
P('SLOW LESLIE ROOM', 2, (), leslie('slow'), None, rev(RM, 0.8, lev=0.35))
P('FAST LESLIE ROOM', 2, (), leslie('fast'), None, rev(RM, 0.8, lev=0.35))
P('SLOW LESLIE PLATE', 2, (), leslie('slow'), None, rev(PL, 1.6, lev=0.3))
P('FAST LESLIE PLATE', 2, (), leslie('fast'), None, rev(PL, 1.6, lev=0.3))
P('LESLIE SLAP ECHO', 2, (), leslie('fast', 0.8), dly(D_MONO, 120, fb=0.15, lev=0.35), rev(RM, 0.7, lev=0.25))
P('LESLIE CHAMBER', 2, (), leslie('slow', 1.2, 0.3, 0.2), None, rev(CH, 1.5, lev=0.35))
P('DEEP SLOW LESLIE', 2, (), leslie('slow', 1.6, 0.5, 0.35), None, None)
P('FAST LESLIE DRY', 2, (), leslie('fast', 1.0, 0.4, 0.3), None, None)
P('LESLIE PING HALL', 2, (), leslie('slow'), dly(D_PING, 340, fb=0.35, lev=0.35), rev(H, 2.0, lev=0.3))
P('LESLIE CHURCH', 2, (), leslie('slow', 0.9, 0.3, 0.2), None, rev(H, 5.0, pre=45, lev=0.5))
# ---- 3: GRAPHIC EQ > DELAY (12)
P('SHORT SLAP', 3, geq(0, 0, 0, 0, 0), None, dly(D_MONO, 85, fb=0.0, damp=8000, lev=0.7))
P('ROCKABILLY SLAP', 3, geq(-3, 0, 3, 2, 0), None, dly(D_MONO, 120, fb=0.2, damp=4500, lev=0.7))
P('QUARTER ECHO', 3, geq(0, 0, 0, -2, -4), None, dly(D_MONO, 500, fb=0.4, damp=3500, lev=0.6))
P('DARK TAPE ECHO', 3, geq(2, 2, 0, -6, -9), None, dly(D_MONO, 380, fb=0.55, damp=1800, lev=0.6))
P('BRIGHT ECHO', 3, geq(-4, -2, 0, 3, 4), None, dly(D_MONO, 300, fb=0.35, damp=9000, lev=0.55))
P('PING PONG 1/8', 3, geq(0, 0, 0, 0, -2), None, dly(D_PING, 180, fb=0.4, damp=5000, lev=0.6))
P('PING PONG LONG', 3, geq(-2, 0, 0, -2, -4), None, dly(D_PING, 600, fb=0.5, damp=3000, lev=0.55))
P('STEREO 3:4 ECHO', 3, geq(0, 0, 0, 0, 0), None, dly(D_STEREO, 300, 400, fb=0.35, fbr=0.4, damp=5000, lev=0.55))
P('DOUBLER', 3, geq(0, -2, 0, 2, 0), None, dly(D_STEREO, 22, 34, fb=0.0, damp=9000, lev=0.8))
P('TELEPHONE ECHO', 3, geq(-9, -6, 6, 0, -9), None, dly(D_MONO, 260, fb=0.45, damp=3000, lev=0.7))
P('LONG REGEN ECHO', 3, geq(0, 0, 0, -3, -6), None, dly(D_MONO, 700, fb=0.78, damp=2600, lev=0.5))
P('RADIO SLAP', 3, geq(-9, 0, 5, 3, -6), None, dly(D_STEREO, 60, 90, fb=0.15, damp=4000, lev=0.75))
# ---- 4: 5-BAND EQ > PITCH > DELAY (12)
P('CHORUS ECHO', 4, eq5(), chorus(0.8), dly(D_MONO, 350, fb=0.35, lev=0.4))
P('STEREO CHORUS', 4, eq5(hs=(7000, 1.5)), chorus(0.7, 10, 3, 0.75, 0.75, 0.5))
P('FLANGE SLAP', 4, eq5(), flange(0.3), dly(D_MONO, 100, fb=0.1, lev=0.4))
P('DEEP FLANGE', 4, eq5(p2=(1200, 2, 1)), flange(0.1, 3.5, 3.0, 0.75))
P('DETUNE DOUBLER', 4, eq5(), detune(8, win=28), dly(D_STEREO, 25, 38, fb=0.0, lev=0.4))
P('DETUNE ECHO', 4, eq5(ls=(120, -2)), detune(11), dly(D_PING, 280, fb=0.35, lev=0.4))
P('PHASER PING', 4, eq5(), phaser(0.45, 250, 3500, 0.5), dly(D_PING, 220, fb=0.4, lev=0.4))
P('SLOW PHASER', 4, eq5(p3=(5000, 2, 1)), phaser(0.1, 200, 4500, 0.65))
P('BRIGHT CHORUS', 4, eq5(ls=(150, -3), p3=(3500, 3, 0.9), hs=(8000, 3)), chorus(1.2, 7, 1.5))
P('DARK FLANGE ECHO', 4, eq5(p3=(4000, -4, 1), hs=(6000, -6)), flange(0.2, 4, 3, 0.5), dly(D_MONO, 420, fb=0.45, damp=2000, lev=0.4))
P('THICK DETUNE', 4, eq5(ls=(100, 2), p1=(300, 2, 1)), detune(18, win=40, dry=0.6, wet=0.8))
P('VIBRATO', 4, eq5(), chorus(5.5, 5, 0.8, 0.0, 0.99, 0.0), direct=0.9)
# ---- 5: EQ > REVERB (24)
P('SMALL ROOM', 5, (), None, None, rev(RM, 0.5, size=0.6, pre=3, lev=0.6))
P('MEDIUM ROOM', 5, (), None, None, rev(RM, 0.9, size=0.8, pre=8, lev=0.6))
P('LARGE ROOM', 5, (), None, None, rev(RM, 1.4, size=1.0, pre=12, lev=0.6))
P('BRIGHT ROOM', 5, eq3(hs=(5000, 4)), None, None, rev(RM, 1.0, size=0.85, damp=9000, lev=0.55))
P('WOOD ROOM', 5, eq3(pk=(500, 2, 0.8), hs=(4000, -4)), None, None, rev(RM, 0.8, size=0.75, damp=3000, lev=0.6))
P('DRUM ROOM', 5, eq3(ls=(100, 3), pk=(3000, 2, 1)), None, None, rev(RM, 0.7, size=0.9, pre=1, er=0.8, lev=0.6))
P('SMALL CHAMBER', 5, (), None, None, rev(CH, 1.0, size=0.7, pre=10, lev=0.55))
P('VOCAL CHAMBER', 5, eq3(ls=(200, -4)), None, None, rev(CH, 1.6, size=0.9, pre=40, lev=0.55))
P('LARGE CHAMBER', 5, (), None, None, rev(CH, 2.4, size=1.0, pre=25, lev=0.55))
P('SMALL PLATE', 5, (), None, None, rev(PL, 1.0, size=0.6, pre=5, lev=0.5))
P('BRIGHT PLATE', 5, eq3(hs=(6000, 4)), None, None, rev(PL, 1.8, damp=10000, pre=15, lev=0.5))
P('DARK PLATE', 5, eq3(hs=(3500, -5)), None, None, rev(PL, 2.2, damp=3000, pre=20, lev=0.55))
P('LONG PLATE', 5, (), None, None, rev(PL, 4.5, pre=30, lev=0.5, mod=10))
P('SMALL HALL', 5, (), None, None, rev(H, 1.6, size=0.7, pre=15, lev=0.5))
P('CONCERT HALL', 5, (), None, None, rev(H, 2.8, pre=35, lev=0.5))
P('BRIGHT HALL', 5, eq3(hs=(6000, 3)), None, None, rev(H, 2.4, damp=8000, pre=25, lev=0.5))
P('DARK HALL', 5, eq3(hs=(3000, -6)), None, None, rev(H, 3.2, damp=2500, pre=30, lev=0.55))
P('CATHEDRAL', 5, eq3(ls=(150, -3)), None, None, rev(H, 9.0, pre=70, lev=0.55, mod=12, mrate=0.4))
P('GATED PLATE', 5, eq3(pk=(250, 3, 1)), None, None, rev(PL, 2.5, pre=2, lev=0.6, gate=(-30, 220, 50)))
P('GATED HALL', 5, eq3(hs=(5000, 3)), None, None, rev(H, 3.5, pre=8, lev=0.65, gate=(-32, 320, 80)))
P('GATED ROOM', 5, (), None, None, rev(RM, 2.0, pre=1, lev=0.6, gate=(-28, 150, 35)))
P('REVERSE SHORT', 5, (), None, None, rev(RV, pre=2, revms=220, lev=0.6))
P('REVERSE LONG', 5, (), None, None, rev(RV, pre=5, revms=600, lev=0.55))
P('AMBIENCE', 5, eq3(hs=(7000, 2)), None, None, rev(RM, 0.35, size=0.5, pre=0, er=0.9, lev=0.6))

SPLIT = {1: 30, 2: 12, 3: 12, 4: 12, 5: 24}

FIELDS = ['cfg', 'neq', 'ptype', 'dtype', 'rtype', 'gate', 'prate', 'prate2', 'pcen', 'pdep', 'pfb', 'pdry', 'pwet',
          'pspread', 'pa0', 'pa1', 'px', 'dtl', 'dtr', 'dfb', 'dfbr', 'ddamp', 'dlev', 'rsize', 'rdec', 'rdamp', 'rbw',
          'rdif1', 'rdif2', 'rddif1', 'rddif2', 'rmod', 'rer', 'rlev', 'dir', 'rpre', 'rert', 'rrev', 'rmrate', 'gthr',
          'grel', 'ghold']


def regen_ms(r):            # time for the delay feedback to fall 70 dB (0 without feedback)
    if not r.get('dtype') or not r.get('dfb'):
        return 0
    fb = max(r['dfb'], r.get('dfbr', 0)) / 32768
    loop = max(r['dtl'], r['dtr']) * (2 if r['dtype'] == D_PING else 1) / FS
    return int(1000 * loop * 70 / (-20 * math.log10(fb)))


def main():
    assert len(PRESETS) == 90, len(PRESETS)
    have = dict(QD_DLY_LEN=DLY_LEN, QD_PRE_LEN=PRE_LEN, QD_PIT_LEN=PIT_LEN, QD_RV_LEN=RV_LEN)
    for k, v in sorted(NEED.items()):
        print('%s: need %d (%d rounded to QD_N), rv_quad.h has %d' % (k, v, -(-v // N) * N, have[k]))
    assert all(v <= have[k] for k, v in NEED.items()), 'a ring in rv_quad.h is too short'
    assert all(have[k] % N == 0 for k in have), 'ring lengths are multiples of QD_N'
    for p in PRESETS:            # the kernels' mz() gains are >= 0 (rv_quad.h)
        assert p.get('rdec', 0) >= 0 and p.get('dfb', 0) >= 0 and p.get('dfbr', 0) >= 0 and p.get('pfb', 0) >= 0, p['name']
    names = [p['name'] for p in PRESETS]
    assert len(set(names)) == 90
    for n in names:
        assert len(n) <= 18 and re.fullmatch(r"[A-Z0-9 .\-/%+#:]+", n), n
    at = 0
    for cfg in range(1, 6):
        for p in PRESETS[at:at + SPLIT[cfg]]:
            assert p['cfg'] == cfg, (p['name'], cfg)
        at += SPLIT[cfg]
    for p in PRESETS:
        assert p.get('ptype', 0) == 0 or p['cfg'] in (1, 2, 4)
        assert p.get('rtype', 0) == 0 or p['cfg'] in (1, 2, 5)
        assert p.get('dtype', 0) == 0 or p['cfg'] in (1, 2, 3, 4)
        assert (p.get('ptype') == P_LESLIE) == (p['cfg'] == 2)
        assert len(p['eq']) <= 5
        p['neq'] = len(p['eq'])
    o = ['/* Generated by gen_quad_presets.py: do not edit. Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the',
         ' * OP-1 firmware permission (see LICENSE and NOTICE). */',
         '#ifndef RV_QUAD_PRESETS_H', '#define RV_QUAD_PRESETS_H',
         '/* split: config 1 presets 0..29, 2: 30..41, 3: 42..53, 4: 54..65, 5: 66..89 */',
         '/* reverb rings per type (none, plate, room, chamber, hall, reverse), tank samples: region start, design delay,',
         ' * ring length */',
         'static const uint16_t qd_rvw[6][12]={']
    o += ['{' + ','.join(str(x) for x in LAY[t][0]) + '},' for t in range(6)]
    o += ['};', 'static const uint16_t qd_rvl[6][12]={']
    o += ['{' + ','.join(str(x) for x in LAY[t][1]) + '},' for t in range(6)]
    o += ['};', 'static const uint16_t qd_rvr[6][12]={']
    o += ['{' + ','.join(str(x) for x in LAY[t][2]) + '},' for t in range(6)]
    o += ['};']
    sin = [int(round(32767 * math.sin(2 * math.pi * i / 256))) for i in range(257)]
    o += ['static const int16_t qd_sin[257]={']
    o += [','.join(str(x) for x in sin[i:i + 16]) + ',' for i in range(0, 257, 16)]
    o += ['};']
    # reverse cluster: 12 taps per side, gains rising (k+1)^1.5, sum of squares 1
    g = [(k + 1) ** 1.5 for k in range(12)]
    nrm = math.sqrt(sum(x * x for x in g))
    o += ['static const int16_t qd_revg[12]={' + ','.join(str(q15(x / nrm * 0.9)) for x in g) + '};']
    o += ['static const char *const qd_names[90]={']
    o += ['"%s",' % n for n in names]
    o += ['};', 'static const qd_preset qd_presets[90]={']
    for i, p in enumerate(PRESETS):
        eq = [list(b) for b in p['eq']] + [[0] * 5] * (5 - len(p['eq']))
        f = ','.join('.%s=%d' % (k, p.get(k, 0)) for k in FIELDS if p.get(k, 0))
        o.append(' /* %2d %s */ {%s,.eq={%s}},' % (i, p['name'], f, ','.join('{' + ','.join(map(str, b)) + '}' for b in eq)))
    o += ['};', '#ifdef QD_TEST_META',
          '/* host test only: designed RT60 (reverse: cluster length) and delay regen time to -70 dB, ms */',
          'static const uint16_t qd_meta_rt[90]={' + ','.join(str(int(p.get('meta_rt', 0) * 1000)) for p in PRESETS) + '};',
          'static const uint32_t qd_meta_regen[90]={' + ','.join(str(regen_ms(p)) for p in PRESETS) + '};',
          '#endif', '#endif', '']
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rv_quad_presets.h')
    with open(path, 'w') as fh:
        fh.write('\n'.join(o))
    print('wrote', path, len(PRESETS), 'presets')


if __name__ == '__main__':
    main()
