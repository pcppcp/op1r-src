/* Synth randomiser core (rn_core.h): generators, type lists, history. */
#ifndef OP1_HOST
#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")   /* no memcpy / memset calls (none in the firmware) */
#endif
#include "rn_core.h"

#define QL_DIVS 11                          /* lfo/quadlfo.c: tempo divisions */
#define QL_NTGT 24                          /* lfo/quadlfo.c: targets */
#define QL_MARK 0xA5                        /* lfo/quadlfo.c: w[7] marker byte */

static void cpy(void *d, const void *s, int n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

void rn_init(rn_t *r, const rn_env *env)
{
    uint8_t *p = (uint8_t *)r;
    for (unsigned i = 0; i < sizeof *r; i++) p[i] = 0;
    r->env = env; r->rng = 0x2545F491u;
}

uint32_t rn_rand(rn_t *r)                   /* xorshift32 */
{
    uint32_t x = r->rng ? r->rng : 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return r->rng = x;
}
static int32_t pick(rn_t *r, int32_t lo, int32_t hi)      /* uniform in [lo, hi]; lo if the range is empty */
{
    return hi <= lo ? lo : lo + (int32_t)(rn_rand(r) % (uint32_t)(hi - lo + 1));
}

void rn_knobs(rn_t *r, int16_t w[8], const rn_range g[8])
{
    for (int k = 0; k < 8; k++) if (!g[k].skip) w[k] = (int16_t)pick(r, g[k].lo, g[k].hi);
}

void rn_env_adsr(rn_t *r, int16_t w[8], const rn_range g[4])
{
    int32_t sp[4];
    for (int k = 0; k < 4; k++) sp[k] = g[k].hi - g[k].lo;
    w[0] = (int16_t)pick(r, g[0].lo, g[0].lo + sp[0] / RN_ATT_DIV);                   /* attack */
    w[2] = (int16_t)pick(r, g[2].lo, g[2].hi);                                         /* sustain */
    int32_t dmin = w[2] < g[2].lo + sp[2] / RN_SUS_DIV ? sp[1] / RN_DEC_DIV : 0;      /* low sustain: no click */
    w[1] = (int16_t)pick(r, g[1].lo + dmin, g[1].hi);                                  /* decay */
    w[3] = (int16_t)pick(r, g[3].lo, g[3].lo + sp[3] / RN_REL_DIV);                   /* release */
}

/* quad words: the lfo/quadlfo.c bit stream (LFO i at bit 30 i, marker at bit 120) */
static void qput(int16_t *w, int pos, int n, uint32_t v)
{
    for (int k = 0; k < n; k++, pos++) {
        uint16_t m = (uint16_t)(1u << (pos & 15)), x = (uint16_t)w[pos >> 4];
        w[pos >> 4] = (int16_t)((v >> k) & 1 ? x | m : x & ~m);
    }
}
uint32_t rn_qget(const int16_t *w, int pos, int n)
{
    uint32_t v = 0;
    for (int k = 0; k < n; k++, pos++) v |= (uint32_t)(((uint16_t)w[pos >> 4] >> (pos & 15)) & 1) << k;
    return v;
}
void rn_quad(rn_t *r, int16_t w[8])
{
    for (int i = 0; i < 8; i++) w[i] = 0;
    int audible = (int)pick(r, 0, 3);                        /* one row targets a knob */
    for (int i = 0; i < 4; i++) {
        int b = 30 * i, sync = (int)pick(r, 0, 1), tgt;
        do tgt = (int)(i == audible ? pick(r, 0, 11) : pick(r, 0, QL_NTGT - 1));
        while (tgt >= 12 + 3 * i && tgt <= 14 + 3 * i);       /* no self target */
        int amt = (int)pick(r, 1, 31) * (pick(r, 0, 1) ? 1 : -1);
        qput(w, b, 6, (uint32_t)(sync ? pick(r, 0, QL_DIVS - 1) : pick(r, 0, 63)));
        qput(w, b + 6, 1, (uint32_t)sync);
        qput(w, b + 7, 3, (uint32_t)pick(r, 0, 5));           /* wave: sine .. sample & hold */
        qput(w, b + 10, 3, (uint32_t)pick(r, 0, 7));          /* smoothing */
        qput(w, b + 13, 3, (uint32_t)pick(r, 0, 7));          /* phase offset */
        qput(w, b + 16, 3, (uint32_t)pick(r, 0, 5));          /* type: VOIC .. RAND */
        qput(w, b + 19, 5, (uint32_t)tgt);
        qput(w, b + 24, 6, (uint32_t)(amt + 32));
    }
    qput(w, 120, 8, QL_MARK);
}

/* "name=[d0, ...];..." rows (factory DB group_concat): names cut at RN_NAME - 1, at most 8 words */
int rn_parse_types(rn_type *out, int max, const char *s)
{
    int n = 0;
    while (*s && n < max) {
        rn_type *t = &out[n];
        int k = 0;
        while (*s && *s != '=' && *s != ';') { if (k < RN_NAME - 1) t->name[k++] = *s; s++; }
        while (k < RN_NAME) t->name[k++] = 0;
        for (int i = 0; i < 8; i++) t->def[i] = 0;
        if (*s == '=') {
            s++;
            for (int i = 0; *s && *s != ';'; ) {
                if (*s == '-' || (*s >= '0' && *s <= '9')) {
                    int neg = *s == '-', v = 0;
                    if (neg) s++;
                    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
                    if (i < 8) t->def[i++] = (int16_t)(neg ? -v : v);
                } else s++;
            }
        }
        if (*s == ';') s++;
        if (t->name[0]) n++;
    }
    return n;
}
int rn_lists(rn_t *r, const char *fx, const char *lfo)
{
    r->nfx = rn_parse_types(r->fx, RN_TYPES, fx);
    int n = rn_parse_types(r->lfo, RN_TYPES, lfo), m = 0;
    for (int i = 0; i < n; i++)                              /* midi left out */
        if (!streq(r->lfo[i].name, "midi")) cpy(&r->lfo[m++], &r->lfo[i], (int)sizeof r->lfo[i]);
    r->nlfo = m;
    return r->nfx && r->nlfo;
}

/* ---- history ---- */
static void scpy(char *d, const char *s)            /* zero padded: steps compare bytewise */
{
    int k = 0;
    while (s[k] && k < RN_NAME - 1) { d[k] = s[k]; k++; }
    while (k < RN_NAME) d[k++] = 0;
}

/* the live sound into L[c]; no history or another engine: start over */
static rn_hist *take(rn_t *r, int slot, rn_step *live)
{
    if ((unsigned)slot >= RN_SLOTS || r->env->read(slot, live)) return 0;
    rn_hist *h = &r->h[slot];
    if (!h->used || !streq(h->L[h->c].type, live->type)) { h->used = 1; h->n = h->c = 0; }
    cpy(&h->L[h->c], live, (int)sizeof *live);
    return h;
}

/* knobs, envelope and FX / LFO types first, their knob words once the types are live */
static int rn_random(rn_t *r, int slot, const rn_step *live, rn_step *s)
{
    rn_range g[8];
    rn_step t;
    r->rng ^= r->env->seed();
    if (r->env->engine_ranges(slot, g)) return -1;
    cpy(s, live, (int)sizeof *s);
    rn_knobs(r, s->knobs, g);
    rn_env_adsr(r, s->adsr, r->adsr);
    const rn_type *fx = &r->fx[pick(r, 0, r->nfx - 1)], *lf = &r->lfo[pick(r, 0, r->nlfo - 1)];
    scpy(s->fx, fx->name); cpy(s->fx_params, fx->def, (int)sizeof s->fx_params); s->fx_on = 1;
    scpy(s->lfo, lf->name); cpy(s->lfo_params, lf->def, (int)sizeof s->lfo_params); s->lfo_on = 1;
    int quad = streq(lf->name, "quad") || streq(lf->name, "fox") || streq(lf->name, "pond");   /* packed words: never the midi ranges */
    if (streq(lf->name, "quad")) rn_quad(r, s->lfo_params);
    if (r->env->apply(slot, s)) return -1;
    if (!r->env->fx_ranges(slot, g)) { rn_knobs(r, s->fx_params, g); r->env->fx_set(slot, s->fx_params); }
    if (!quad && !r->env->lfo_ranges(slot, g)) { rn_knobs(r, s->lfo_params, g); r->env->lfo_set(slot, s->lfo_params); }
    if (!r->env->read(slot, &t)) cpy(s, &t, (int)sizeof t);
    return 0;
}

int rn_lift(rn_t *r, int slot)
{
    rn_step live;
    if (r->env->busy()) return 0;                               /* a save is writing the sound */
    rn_hist *h = take(r, slot, &live);
    if (!h || !h->c || r->env->apply(slot, &h->L[h->c - 1])) return 0;
    h->c--;
    return 1;
}

int rn_drop(rn_t *r, int slot)
{
    rn_step live, s;
    if (r->env->busy()) return 0;                               /* a save is writing the sound */
    if (!r->nfx || !r->nlfo) return 0;                         /* type lists not read yet */
    for (int k = 0; k < 4; k++) if (r->adsr[k].hi <= r->adsr[k].lo) return 0;   /* envelope ranges not filled */
    rn_hist *h = take(r, slot, &live);
    if (!h) return 0;
    if (h->c < h->n) {                                         /* forward */
        if (r->env->apply(slot, &h->L[h->c + 1])) return 0;
        h->c++;
        return 1;
    }
    if (rn_random(r, slot, &live, &s)) return 0;
    if (h->n == RN_KEEP) {                                     /* full: drop the oldest random step */
        for (int i = 1; i < RN_KEEP; i++) cpy(&h->L[i], &h->L[i + 1], (int)sizeof s);
        h->n--;
    }
    h->c = ++h->n;
    cpy(&h->L[h->c], &s, (int)sizeof s);
    return 1;
}
