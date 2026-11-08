/* "pond" LFO (docs/64): four competing species, in the quad LFO's hooks and code paths (marker 0xA7). Included at
 * the end of lfo/quadlfo.c: the words, the DSP (pond_lv, wave 10..13), the controls and the screen.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */

/* pond words <-> fields (docs/64) */
typedef struct { uint8_t rate, sync, retrig, chaos, tgt[4]; int8_t amt[4]; } pond_set;
static void pond_get(const int16_t *w, pond_set *f)
{
    f->rate = (uint8_t)ql_get(w, 0, 6); f->sync = (uint8_t)ql_get(w, 6, 1); f->retrig = (uint8_t)ql_get(w, 7, 1);
    f->chaos = (uint8_t)ql_get(w, 8, 5);
    for (int k = 0; k < 4; k++) {
        int t = (int)ql_get(w, 13 + 11 * k, 5);
        f->tgt[k] = (uint8_t)(t >= 12 && t < QL_P2 ? 0 : clampq(t, 0, QL_NTGT - 1));
        f->amt[k] = (int8_t)clampq((int)ql_get(w, 18 + 11 * k, 6) - 32, -31, 31);
    }
    if (f->sync) f->rate = (uint8_t)clampq(f->rate, 0, QL_DIVS - 1);
}
static void pond_put(const pond_set *f, int16_t *w)
{
    ql_put(w, 0, 6, f->rate); ql_put(w, 6, 1, f->sync); ql_put(w, 7, 1, f->retrig); ql_put(w, 8, 5, f->chaos);
    for (int k = 0; k < 4; k++) { ql_put(w, 13 + 11 * k, 5, f->tgt[k]); ql_put(w, 18 + 11 * k, 6, (uint32_t)(f->amt[k] + 32)); }
    ql_put(w, 120, 8, POND_MARK);
}
static void pond_dec(const int16_t *w, int i, ql_lfo *o)
{
    pond_set f; pond_get(w, &f);
    o->rate = f.rate; o->sync = f.sync; o->smooth = 0; o->off = 0; o->tl = o->tk = 0; o->phoff = 0;
    o->type = f.retrig ? QT_MONO : QT_FREE; o->bloom = f.chaos;
    o->inc = o->sync ? ql_div_inc(o->rate) : ql_inc((o->rate << 9) | 0x100);
    o->wave = (uint8_t)(10 + i); o->tgt = f.tgt[i]; o->amt = f.amt[i];
    o->a128 = (int16_t)(o->amt * 127 / 31);
}


/* ======================= "pond" LFO: four competing species (docs/64) =======================
 * Competitive Lotka-Volterra after Vano et al. 2006: dx_i/dt = r_i x_i (1 - sum_j a_ij x_j), off-diagonal a_ij scaled by
 * s = 0.85 .. 1.05 (chaos 0..31: from a gentle regular oscillation to chaos). Populations Q20 in LFO 0's state (lx, ly,
 * lz, d1; d2 = started; Q14 lost small species: their growth rounded to 0, the system froze), Euler sub-steps <= 0.02
 * time units (max 32), floor 0.001 (no extinction). One LFO cycle = the
 * mean oscillation period at that chaos (pond_T), output k = (x_k - mean_k) / max swing_k (pond_mean, pond_ext): calm
 * = small swings, full chaos ~ +-1. Words: rate 6 | sync 1 | retrig 1 | chaos 5 | 4 x (target 5 | amount 6 +32),
 * marker 0xA7. Decoded as four quad LFOs, waves 10..13 (10 integrates, 11..13 read), FREE / MONO. */
static const int16_t pond_r[4] = { 4096, 2949, 6267, 5202 };          /* r_i Q12 */
static const int16_t pond_a[4][4] = { { 4096, 4465, 6226, 0 }, { 0, 4096, 1802, 5571 }, { 9544, 0, 4096, 1925 },
                                      { 4956, 2089, 1434, 4096 } };    /* a_ij Q12 */
static const uint16_t pond_T[33] = { 2578, 2572, 2564, 2556, 2547, 2537, 2526, 2514, 2502, 2489, 2475, 2460, 2444, 2428,
    2415, 2402, 2388, 2371, 2352, 2333, 2311, 2312, 2282, 2154, 2030, 1924, 1956, 1968, 1992, 1930, 1937, 1929, 1939 };   /* Q6 */
static const int16_t pond_mean[33][4] = { {5437,7655,2984,6586},{5419,7658,2934,6545},{5397,7655,2893,6509},{5375,7645,2856,6477},
    {5361,7639,2809,6440},{5343,7644,2761,6399},{5317,7636,2728,6369},{5302,7625,2687,6335},{5284,7632,2641,6295},{5258,7618,2612,6269},
    {5247,7622,2562,6227},{5220,7616,2532,6198},{5207,7616,2487,6160},{5179,7611,2460,6131},{5163,7608,2421,6096},{5134,7607,2390,6067},
    {5113,7592,2361,6042},{5085,7584,2337,6016},{5070,7571,2304,5987},{5046,7557,2279,5963},{5022,7548,2253,5937},{5003,7545,2220,5905},
    {4975,7527,2203,5887},{4955,7537,2161,5851},{4934,7520,2140,5828},{4914,7508,2114,5803},{4895,7497,2089,5777},{4873,7494,2061,5749},
    {4851,7476,2043,5726},{4829,7470,2014,5703},{4805,7465,1990,5678},{4786,7440,1974,5660},{4762,7420,1956,5644} };   /* Q14 */
static const int16_t pond_ext[4] = { 6972, 5838, 4794, 4219 };       /* max swing at full chaos, Q14 */
static volatile int16_t pond_trail[3][48][2];                         /* (x1 - x3, x2 - x4) Q14 per 1/48 cycle */
static volatile uint8_t pond_dom[3][48], pond_head[3];
static int32_t *pond_x(ql_st *s, int k) { return k == 0 ? &s->lx : k == 1 ? &s->ly : k == 2 ? &s->lz : &s->d1; }
static int32_t pond_lv(ql_st *s, const ql_lfo *o, uint32_t adv, int i)
{
    ql_st *s0 = s - i;                                         /* species k: LFO k reads LFO 0's state (sh_st[pos][0]) */
    int q = o->bloom > 31 ? 31 : o->bloom;
    if (i == 0) {
        if (!s->d2) { s->lx = 314573; s->ly = 419430; s->lz = 314573; s->d1 = 524288; s->d2 = 1; }   /* (0.3, 0.4, 0.3, 0.5) Q20 */
        uint32_t dt = (uint32_t)(((uint64_t)adv * pond_T[q]) >> 26);     /* time units Q12 (T Q6) */
        int n = (int)((dt + 81) / 82);                                   /* sub-steps <= 0.02 */
        if (n > 32) { n = 32; dt = 32 * 82; }
        int32_t h = n ? (int32_t)(dt / (uint32_t)n) : 0, sc = 3482 + q * 26;   /* s Q12: 0.85 .. 1.05 */
        int32_t x[4] = { s->lx, s->ly, s->lz, s->d1 };
        for (int st = 0; st < n; st++) {
            int32_t d[4];
            for (int a = 0; a < 4; a++) {
                int32_t sum = 0;
                for (int b = 0; b < 4; b++) {
                    int32_t c = a == b ? pond_a[a][b] : (pond_a[a][b] * sc) >> 12;
                    sum += (c * (x[b] >> 8)) >> 12;              /* Q12 */
                }
                int32_t rx = ((x[a] >> 4) * pond_r[a]) >> 8;     /* r x, Q20 */
                d[a] = ((rx >> 6) * (4096 - sum)) >> 6;          /* Q20 per time unit */
            }
            for (int a = 0; a < 4; a++) { x[a] += (d[a] * h) >> 12; if (x[a] < 1049) x[a] = 1049; if (x[a] > 2097152) x[a] = 2097152; }
        }
        s->lx = x[0]; s->ly = x[1]; s->lz = x[2]; s->d1 = x[3];
        uint32_t nph = s->ph, oph = nph - adv;
        if (adv && (uint32_t)(((uint64_t)oph * 48) >> 32) != (uint32_t)(((uint64_t)nph * 48) >> 32)) {   /* every 1/48 cycle */
            int pg = (int)((s - &sh_st[0][0]) / 4) % 3, hd = (pond_head[pg] + 1) % 48, dm = 0;
            for (int a = 1; a < 4; a++) if (x[a] > x[dm]) dm = a;
            pond_trail[pg][hd][0] = (int16_t)((x[0] - x[2]) >> 6); pond_trail[pg][hd][1] = (int16_t)((x[1] - x[3]) >> 6);
            pond_dom[pg][hd] = (uint8_t)dm; pond_head[pg] = (uint8_t)hd;
        }
    }
    int32_t out = (int32_t)(((int64_t)((*pond_x(s0, i) >> 6) - pond_mean[q][i]) * 32767) / pond_ext[i]);
    return out > 32767 ? 32767 : out < -32767 ? -32767 : out;
}

static uint8_t pond_tgt_step(int t, int st, int pos)                /* targets as quad (page 2 after page 1), no LFO ones */
{
    static const uint8_t order[18] = { 0, 1, 2, 3, 24, 25, 26, 4, 5, 6, 7, 8, 9, 10, 11, 27, 28, 29 };
    int p = 0, d = st > 0 ? 1 : -1;
    while (p < 17 && order[p] != t) p++;
    for (int n = st < 0 ? -st : st; n--; )
        do p = wrapq(p + d, 18); while ((pos && order[p] >= QL_P2 && order[p] < QL_P2 + 3) || !ql_tgt_ok(pos, order[p]));   /* drums: no engine page 2; master: FX only */
    return order[p];
}

/* pond controls: presses select species 1..4 (ql_sel, as quad); blue speed, green chaos, white amount, red target of
 * the selected species; Shift: blue free / sync, green mode free / retrigger */
static void pond_turn(int16_t *w, int pos, int knob, int st, int shift)
{
    pond_set f; pond_get(w, &f);
    int k = ql_sel[pos > 2 ? 0 : pos] & 3;
    if (!shift) switch (knob) {
        case 0: f.rate = (uint8_t)clampq(f.rate + st, 0, f.sync ? QL_DIVS - 1 : 63); break;
        case 1: f.chaos = (uint8_t)clampq(f.chaos + st, 0, 31); break;
        case 2: f.amt[k] = (int8_t)clampq(f.amt[k] + st, -31, 31); break;
        default: f.tgt[k] = pond_tgt_step(f.tgt[k], st, pos); break;
    } else switch (knob) {
        case 0:
            if (st > 0 && !f.sync) { f.sync = 1; f.rate = 4; } else if (st < 0 && f.sync) { f.sync = 0; f.rate = 31; }
            break;
        case 1: f.retrig = (uint8_t)(st > 0); break;
        default: break;
    }
    pond_put(&f, w);
}

/* ---- pond LFO screen helpers: bottom row in the stock font (as dddd), colour dots */
static const uint8_t pd_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, pd_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     pd_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, pd_green[4] = { 0x00, 0xED, 0x95, 0xFF };
static const uint8_t *const pd_col[4] = { pd_blue, pd_green, pd_white, pd_red };
enum { PT_LAB, PT_VAL = 4, PT_N = 8 };
static uint32_t pd_tx[PT_N][OP1_TEXT_SIZE / 4];
static char pd_shown[PT_N][14];
static uint8_t pd_made;
static void pd_txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (!pd_made) {
        for (int j = 0; j < 4; j++) {
            op1_text_ctor(pd_tx[PT_LAB + j], OP1_Q10(8 + 78 * j), 114, 12, pd_col[j]);
            op1_text_ctor(pd_tx[PT_VAL + j], OP1_Q10(8 + 78 * j), 127, 12, pd_white);
        }
        pd_made = 1;
    }
    int same = 1;
    for (int j = 0; j < 14; j++) { if (pd_shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(pd_tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) pd_shown[i][j] = s[j]; pd_shown[i][j] = 0; }
    op1_text_draw(pd_tx[i], r, ident);
}
static void pd_dot(void *r, int32_t x, int32_t y, int32_t sz, const uint8_t *c)
{
    qline(r, x - sz / 2, y, x + sz / 2, y, sz * 7 / 10, c);
    qline(r, x, y - sz / 2, x, y + sz / 2, sz * 7 / 10, c);
}
static char *pd_cat(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
/* stock font: lowercase only (uppercase letters render as other glyphs, e.g. T): the division names ("1/4T") lowered */
static char *pd_cat_lc(char *p, const char *s) { while (*s) { char c = *s++; *p++ = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; } *p = 0; return p; }
static char *pd_pct(char *p, int v31, int sign)              /* -31..31 -> "+80" / "80%" */
{
    int v = (v31 * 100 + (v31 < 0 ? -15 : 15)) / 31;
    if (sign) *p++ = v < 0 ? '-' : '+';
    p = ql_num(p, v < 0 ? -v : v);
    if (!sign) p = pd_cat(p, "%");
    return p;
}
/* a target as the value of bottom-row column col (text slot ti): "inst" / "fx" + a dot in the knob's colour (page 2:
 * two dots above each other, as the quad LFO's target column), envelope stages as text ("env a") */
static void pd_tgt(void *r, int ti, int col, int t, int pos)
{
    static const char ec[4] = { 'a', 'd', 's', 'r' };
    char b[8];
    int k = -1, p2 = 0;
    const char *wd;
    (void)pos;
    if (t < 4) { wd = "inst"; k = t; }
    else if (t < 8) { b[0] = 'e'; b[1] = 'n'; b[2] = 'v'; b[3] = ' '; b[4] = ec[t - 4]; b[5] = 0; wd = b; }
    else if (t < 12) { wd = "fx"; k = t - 8; }
    else if (t < QL_P2 + 3) { wd = "inst"; k = t - QL_P2; p2 = 1; }
    else { wd = "fx"; k = (t - QL_P2 - 3) % 3; p2 = 1; }
    pd_txt(r, ti, wd);
    if (k < 0) return;
    int n = 0; while (wd[n]) n++;
    int32_t x = OP1_Q10(8 + 78 * col) + n * OP1_Q10(15) / 2 + OP1_Q10(6), y = OP1_Q10(127 + 6);   /* stock font: ~7.5 px / char */
    if (p2) { pd_dot(r, x, y - OP1_Q10(4), OP1_Q10(5), pd_col[k]); pd_dot(r, x, y + OP1_Q10(4), OP1_Q10(5), pd_col[k]); }
    else pd_dot(r, x, y, OP1_Q10(6), pd_col[k]);
}
/* ---- pond LFO screen (docs/64): species 1 - 3 across, 2 - 4 up; the last 48 points (1/48 cycle apart) as dots in the
 * colour of the species that led then, fading with age; the current point larger; top left the four populations as
 * bars (selected species marked); bottom row in the stock font */
#define PD_CX 165
#define PD_CY 52
static void pd_screen_init(void *scr) { (void)scr; }
static void ml_off_overlay(void *r, int pos);
static void pd_screen_update(void *scr, int dt)
{
    int pos = PAGE_POS(scr), pg = pos > 2 ? 0 : pos, sel = ql_sel[pg] & 3;
    const int16_t *w = PAGE_WORDS(pos);
    pond_set f; pond_get(w, &f);
    if (LFO_ON(pg) && blocks_q16(READ_CYCLES(), ph_cyc_all[pg]) > (4u << 16)) {   /* audio idle: advance here (as quad) */
        ql_lfo Lm[4]; ql_dec_shared(w, pg, Lm); ql_advance_pos(pg, Lm);
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    int hd = pond_head[pg];
    for (int k = 47; k >= 0; k--) {
        int j = (hd - k + 48) % 48;
        int32_t a = pond_trail[pg][j][0], b = pond_trail[pg][j][1];
        if (!a && !b) continue;
        const uint8_t *cc = pd_col[pond_dom[pg][j] & 3];
        int lv = 256 - k * 5;
        uint8_t c[4] = { (uint8_t)(cc[0] * lv >> 8), (uint8_t)(cc[1] * lv >> 8), (uint8_t)(cc[2] * lv >> 8), 0xFF };
        int32_t x = OP1_Q10(PD_CX) + (int32_t)(((int64_t)a * OP1_Q10(128)) / 9830);   /* +-0.6 -> +-128 px */
        int32_t y = OP1_Q10(PD_CY) - (int32_t)(((int64_t)b * OP1_Q10(44)) / 11469);   /* +-0.7 -> +-44 px */
        pd_dot(r, x, y, OP1_Q10(k ? 3 : 6), k ? c : cc);
    }
    {   /* the four populations (Q14, 0.5 -> 24 px), selected one marked */
        const ql_st *st = &sh_st[pg][0];
        int32_t xs[4] = { st->lx, st->ly, st->lz, st->d1 };
        for (int k = 0; k < 4; k++) {
            int32_t hgt = st->d2 ? (xs[k] >> 6) * 48 / 16384 : 0; if (hgt > 40) hgt = 40;
            int32_t bx = OP1_Q10(8 + 7 * k);
            if (hgt > 0) qline(r, bx, OP1_Q10(46), bx, OP1_Q10(46 - hgt), OP1_Q10(4), pd_col[k]);
            if (k == sel) qline(r, bx - OP1_Q10(3), OP1_Q10(50), bx + OP1_Q10(3), OP1_Q10(50), OP1_Q10(2), pd_col[k]);
        }
    }
    for (int i = 0; i < 4; i++) qline(r, OP1_Q10(8 + 78 * i), OP1_Q10(110), OP1_Q10(78 + 78 * i), OP1_Q10(110), 0x800, pd_col[i]);
    char t[16], *p;
    if (!SHIFT_HELD()) {
        pd_txt(r, PT_LAB, "speed");
        if (f.sync) pd_cat_lc(t, ql_div_name[f.rate]);
        else { ql_hz_text(t, ql_inc((f.rate << 9) | 0x100)); char *e = t; while (*e) e++; pd_cat(e, " hz"); }
        pd_txt(r, PT_VAL, t);
        pd_txt(r, PT_LAB + 1, "chaos"); pd_pct(t, f.chaos, 0); pd_txt(r, PT_VAL + 1, t);
        p = pd_cat(t, "amount "); *p++ = (char)('1' + sel); *p = 0; pd_txt(r, PT_LAB + 2, t);
        pd_pct(t, f.amt[sel], 1); pd_txt(r, PT_VAL + 2, t);
        p = pd_cat(t, "target "); *p++ = (char)('1' + sel); *p = 0; pd_txt(r, PT_LAB + 3, t);
        pd_tgt(r, PT_VAL + 3, 3, f.tgt[sel], pos);
    } else {
        pd_txt(r, PT_LAB, "clock"); pd_txt(r, PT_VAL, f.sync ? "sync" : "free");
        pd_txt(r, PT_LAB + 1, "mode"); pd_txt(r, PT_VAL + 1, f.retrig ? "retrig" : "free");
        pd_txt(r, PT_LAB + 2, ""); pd_txt(r, PT_VAL + 2, "");
        pd_txt(r, PT_LAB + 3, ""); pd_txt(r, PT_VAL + 3, "");
    }
    ml_off_overlay(r, pos);
}
const op1_screen_t SCREEN_pondlfo = { pd_screen_init, pd_screen_update };
