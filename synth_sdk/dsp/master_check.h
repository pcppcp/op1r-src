/* OP1R_MASTER=check|stress (docs/81 3b-3): each of our master section's loops against its model (dsp/mix.c,
 * dsp/mon.c, dsp/cw.c: each proven against R246's loop), per block, from the same state. Check builds switch the
 * master monitor on (no sample buffer); stress builds randomise the inputs, the jack flag and the sync variant.
 * master_check: blocks, differing (mix, mon, cw), the first one (what 0 mix 1 mon 2 cw, index, both values),
 * speaker / sync / monitor blocks. */
void mix_model(int16_t *L, int16_t *R, int16_t *ramp, int16_t *met);
int16_t mon_model(int16_t *y, int16_t peak);
int16_t cw_model(int32_t *out, int16_t cur, int16_t step, int sync);
int16_t spk_model(int32_t *out, int16_t cur, int16_t step, int32_t *st);
struct { uint32_t blocks, bad[3], first_what, first_i, first_orig, first_model, spk, sync, mon; } master_check;
static int16_t ck_l[64], ck_r[64], ck_ramp[3], ck_met, ck_mon[64], ck_peak, ck_cur;
static int32_t ck_out[128], ck_st[2];
static int ck_idx, ck_spk;
static __attribute__((noinline)) void ck_bad(int what, int i, uint32_t o, uint32_t m)
{
    if (!master_check.bad[0] && !master_check.bad[1] && !master_check.bad[2]) {
        master_check.first_what = (uint32_t)what; master_check.first_i = (uint32_t)i;
        master_check.first_orig = o; master_check.first_model = m;
    }
    master_check.bad[what]++;
}
#ifdef MASTER_STRESS
static uint32_t ck_rs = 0x31415926;
static uint32_t ck_rnd(void) { ck_rs ^= ck_rs << 13; ck_rs ^= ck_rs >> 17; ck_rs ^= ck_rs << 5; return ck_rs; }
static int32_t ck_val(void) { uint32_t r = ck_rnd(); switch (r >> 29) { case 0: return 0x7FFFFFFF; case 1: return (int32_t)0x80000000; case 2: return 0; default: return (int32_t)(r * 2654435761u); } }
#endif
static __attribute__((noinline)) void ck_mix_pre(void)
{
#ifdef MASTER_STRESS
    const int ix = D32(0xFF802964);
    for (int i = 0; i < 64; i++) { W16(0xFF800BB4 + 2 * i) = (int16_t)ck_val(); W16(0xFF800D34 + 2 * i) = (int16_t)ck_val(); W16(0xFF800DB4 + 2 * i) = (int16_t)ck_val(); }
    W16(0xFF801928 + 2 * ix) = (int16_t)ck_val(); W16(0xFF801930 + 2 * ix) = (int16_t)ck_val(); W16(0xFF8018B4 + 2 * ix) = (int16_t)ck_val();
    W16(0x01B4EF6E) = (int16_t)ck_val(); W16(0xFF801080) = (int16_t)ck_val(); W16(0xFF801082) = (int16_t)ck_val();
    W16(0xFF802990 + 2 * ix) = (int16_t)ck_val(); B8(0xFF80103C) = (uint8_t)(ck_rnd() & 1);
    B8(0x018B621D) = (uint8_t)(ck_rnd() & 1);
#endif
    D32(0xFF8010B0) = 1; D32(0xFF8010AC) = 0;               /* the monitor on, no sampling */
    for (int i = 0; i < 64; i++) { ck_l[i] = W16(0xFF800D34 + 2 * i); ck_r[i] = W16(0xFF800DB4 + 2 * i); }
    for (int i = 0; i < 3; i++) ck_ramp[i] = W16(0xFF801080 + 2 * i);
    ck_idx = D32(0xFF802964); ck_met = W16(0xFF802990 + 2 * ck_idx);
    mix_model(ck_l, ck_r, ck_ramp, &ck_met);                /* the slot meter is the driver's (C), not compared */
}
static __attribute__((noinline)) void ck_mix_post(void)
{
    master_check.blocks++;
    for (int i = 0; i < 64; i++) {
        if (W16(0xFF800D34 + 2 * i) != ck_l[i]) { ck_bad(0, i, (uint16_t)W16(0xFF800D34 + 2 * i), (uint16_t)ck_l[i]); return; }
        if (W16(0xFF800DB4 + 2 * i) != ck_r[i]) { ck_bad(0, 64 + i, (uint16_t)W16(0xFF800DB4 + 2 * i), (uint16_t)ck_r[i]); return; }
    }
    for (int i = 0; i < 3; i++) if (W16(0xFF801080 + 2 * i) != ck_ramp[i]) { ck_bad(0, 128 + i, (uint16_t)W16(0xFF801080 + 2 * i), (uint16_t)ck_ramp[i]); return; }
}
static __attribute__((noinline)) void ck_mon_pre(void)
{
#ifdef MASTER_STRESS
    for (int i = 0; i < 64; i++) { D32(0xFF800E34 + 4 * i) = ck_val(); D32(0xFF800F34 + 4 * i) = ck_val(); }
#endif
    master_check.mon++;
    ck_peak = mon_model(ck_mon, W16(0xFF801084));
}
static __attribute__((noinline)) void ck_mon_post(void)
{
    for (int i = 0; i < 64; i++) if (W16(0xFF800BB4 + 2 * i) != ck_mon[i]) { ck_bad(1, i, (uint16_t)W16(0xFF800BB4 + 2 * i), (uint16_t)ck_mon[i]); return; }
    if (W16(0xFF801084) != ck_peak) ck_bad(1, 64, (uint16_t)W16(0xFF801084), (uint16_t)ck_peak);
}
static __attribute__((noinline)) void ck_cw_pre(int32_t *out, int sync)
{
#ifdef MASTER_STRESS
    for (int i = 0; i < 64; i++) { D32(0xFF800E34 + 4 * i) = ck_val(); D32(0xFF800F34 + 4 * i) = ck_val(); }
    W16(0x01DE4F70) = (int16_t)ck_val(); W16(0x01DE4F72) = (int16_t)(ck_rnd() & 1 ? ck_val() : (int32_t)(ck_rnd() % 13) - 6);
#endif
    ck_spk = !B8(0x018B621D);
    master_check.spk += ck_spk; master_check.sync += sync;
    if (ck_spk) {
        for (int i = 0; i < 64; i++) ck_out[2 * i + 1] = out[2 * i + 1];
        ck_st[0] = D32(0x01AAAC50); ck_st[1] = D32(0x01AAAC54);
        ck_cur = spk_model(ck_out, W16(0x01DE4F70), W16(0x01DE4F72), ck_st);
    } else
        ck_cur = cw_model(ck_out, W16(0x01DE4F70), W16(0x01DE4F72), sync);
}
static __attribute__((noinline)) void ck_cw_post(int32_t *out)
{
    for (int i = 0; i < 128; i++) if (out[i] != ck_out[i]) { ck_bad(2, i, (uint32_t)out[i], (uint32_t)ck_out[i]); return; }
    if (W16(0x01DE4F70) != ck_cur) { ck_bad(2, 128, (uint16_t)W16(0x01DE4F70), (uint16_t)ck_cur); return; }
    if (ck_spk && (D32(0x01AAAC50) != ck_st[0] || D32(0x01AAAC54) != ck_st[1])) ck_bad(2, 129, (uint32_t)D32(0x01AAAC50), (uint32_t)ck_st[0]);
}
#define CK_MIX_PRE() ck_mix_pre()
#define CK_MIX_POST() ck_mix_post()
#define CK_MON_PRE() ck_mon_pre()
#define CK_MON_POST() ck_mon_post()
#define CK_CW_PRE(o, f) ck_cw_pre(o, f)
#define CK_CW_POST(o) ck_cw_post(o)
