#!/usr/bin/env python3
"""Generate the exponential phase warp table for the brass synth engine.

The warp function (from user's code):
  ph_f_sym = 2.0 * ph_f - 1.0    # [-1, 1]
  x' = (exp(timbre * |x|) - 1) / (exp(timbre) - 1)
  ph_f_sym' = sign(x) * x'
  ph_f_warped = 0.5 * ph_f_sym' + 0.5    # back to [0, 1]

At timbre=0: ph_f_warped = ph_f (identity, pure sine)
At timbre=20: ph_f_warped ≈ 0 for ph_f near 0.5, ≈ 1 for ph_f near 0 or 1 (delta at edges)

Output: C array of Q15 values in [0, 32767] representing [0, 1].
"""
import math

NUM_SAMPLES = 64  # 6-bit phase resolution
MAX_TIMBRE = 20
NUM_COLUMNS = 256

def warp(ph_f, timbre):
    """Warp normalized phase ph_f in [0,1] toward the center by timbre."""
    if abs(timbre) < 0.01:
        return ph_f
    ph_f_sym = 2.0 * ph_f - 1.0  # [-1, 1]
    abs_value = abs(ph_f_sym)
    denom = math.exp(-timbre) - 1.0
    if abs(denom) < 1e-10:
        return ph_f
    numerator = math.exp(-timbre * abs_value) - 1.0
    x_warped = numerator / denom
    if ph_f_sym < 0:
        x_warped = -x_warped
    ph_f_warped = 0.5 * x_warped + 0.5
    return max(0.0, min(1.0, ph_f_warped))

def to_q15(v):
    """Convert float in [0, 1] to Q15 int."""
    v = max(0.0, min(1.0, v))
    return int(v * 32767)

lines = []
lines.append("/* Exponential phase warp table: warp_table[s][t] = Q15 warped phase in [0,1] */")
lines.append("/* s = 0..63 (phase index), t = 0..256 (timbre 0..20 mapped from column) */")
lines.append("static const int16_t warp_table[64][257] = {")

for s in range(NUM_SAMPLES):
    ph_f = s / (NUM_SAMPLES - 1)  # 0 to 1
    lines.append("    {  // s=%d, ph_f=%.4f" % (s, ph_f))
    
    vals = []
    for t in range(NUM_COLUMNS + 1):
        timbre = t * MAX_TIMBRE / NUM_COLUMNS
        ph_f_warped = warp(ph_f, timbre)
        vals.append("%6d" % to_q15(ph_f_warped))
        if len(vals) == 8:
            lines.append("        " + ", ".join(vals) + ",")
            vals = []
    if vals:
        lines.append("        " + ", ".join(vals) + ",")
    lines.append("    },")

lines.append("};")

print("\n".join(lines))
