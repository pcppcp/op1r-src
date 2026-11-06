/* Host render tool for SISTER's DSP (source-only: not the R246 chain, no envelope, LFO or mixer).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
 *   sister_render IN.raw OUT.raw q0 q1 q2 q3 key_cents [mono] [AUTOMATION]
 * IN / OUT: interleaved stereo int16 at SI_FS (44.1 kHz). q0..q3: the macros 0..32767; key_cents -2400..2400.
 * AUTOMATION: a text file of "block knob value" lines (knob 0..3, 4 = key cents), applied from that block on. */
#include "sister_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static si_state s;
int main(int argc, char **argv)
{
 if (argc < 8) { fprintf(stderr, "usage: sister_render IN OUT q0 q1 q2 q3 key [mono] [automation]\n"); return 2; }
 FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb"), *au = argc > 9 ? fopen(argv[9], "r") : 0;
 if (!in || !out || (argc > 9 && !au)) return 1;
 int32_t q[4];
 for (int i = 0; i < 4; i++) q[i] = atoi(argv[3 + i]);
 int32_t key = atoi(argv[7]);
 const int mono = argc > 8 && atoi(argv[8]);
 long ab = -1; int ak = 0, av = 0;
 if (au && fscanf(au, "%ld %d %d", &ab, &ak, &av) != 3) ab = -1;
 si_init(&s);
 int16_t buf[128], l[64], r[64];
 for (long b = 0; fread(buf, 2, 128, in) == 128; b++) {
  while (ab >= 0 && ab <= b) {
   if (ak < 4) q[ak] = av; else key = av;
   if (fscanf(au, "%ld %d %d", &ab, &ak, &av) != 3) ab = -1;
  }
  for (int i = 0; i < 64; i++) { l[i] = buf[2 * i]; r[i] = buf[2 * i + 1]; }
  if (mono) { for (int i = 0; i < 64; i++) l[i] = (int16_t)((l[i] + r[i]) >> 1); si_process(&s, l, 0, q, key); memcpy(r, l, sizeof r); }
  else si_process(&s, l, r, q, key);
  for (int i = 0; i < 64; i++) { buf[2 * i] = l[i]; buf[2 * i + 1] = r[i]; }
  fwrite(buf, 2, 128, out);
 }
 fclose(out);
 return 0;
}
