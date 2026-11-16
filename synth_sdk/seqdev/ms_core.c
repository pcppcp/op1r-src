/* MIDI sequencer device core (docs/39, ms_core.h). Threads in the firmware: ms_block, ms_note_in, ms_cc_in in the
 * audio thread (the glue queues inputs from other threads); ms_service and the editing in the main loop, every change
 * of slot contents or chains there inside env->irq_off (short: one slot at a time). */
#ifndef OP1_HOST
#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")   /* no memmove / memset calls (none in the firmware) */
#endif
#include "ms_core.h"

#define MS_MAGIC   0x534D                   /* 'MS' */
#define MS_HMAGIC  0x5145534Du              /* 'MSEQ' */
#define MS_JUMP    (64 * 16)                /* a position step bigger than this per block is a jump (locate / wrap) */
#define MS_RESERVE 8                        /* free RAM buffers kept for the audio thread (new / split slots) */
#define MS_CC_THIN 441                      /* recorded CC values: at most one per controller per 10 ms */

static void cpy(void *d, const void *s, int n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void clr(void *d, int n) { uint8_t *a = d; while (n--) ((volatile uint8_t *)a)[n] = 0; (void)a; }

uint32_t ms_crc(const void *p, int n)
{
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *b++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}
static uint32_t slot_crc(ms_slot *b)
{
    uint32_t keep = b->crc, c;
    b->crc = 0;
    c = ms_crc(b, 16 + 8 * (b->n > MS_SLOT_EV ? MS_SLOT_EV : b->n));
    b->crc = keep;
    return c;
}

void ms_init(ms_t *s, const ms_env *env)
{
    clr(s, (int)sizeof *s);
    s->env = env;
    s->h.magic = MS_HMAGIC; s->h.ver = 1;
    for (int t = 0; t < MS_TRACKS; t++) { s->h.ch[t] = (uint8_t)t; for (int p = 0; p < MS_PAGES; p++) s->h.first[t][p] = MS_NONE; }
    for (int i = 0; i < MS_RAM; i++) s->ram_slot[i] = MS_NONE;
    for (int i = 0; i < MS_SLOTS; i++) s->where[i] = 0xFF;
    s->prev = -1; s->clip_len = -1; s->scan = -1;
    for (int c = 0; c < 128; c++) s->cc_last[c] = -MS_CC_THIN;
}

static void scan_start(ms_t *s) { clr(s->seen, (int)sizeof s->seen); s->scan = 0; s->scan_sl = MS_NONE; s->scan_gen = s->gen; }
static int head_check(ms_head *h)           /* 0 valid (then sanitized) */
{
    uint32_t c = h->crc;
    h->crc = 0;
    if (h->magic != MS_HMAGIC || h->ver != 1 || ms_crc(h, (int)sizeof *h) != c) return 1;
    h->crc = c;
    for (int t = 0; t < MS_TRACKS; t++) {
        if (h->dest[t] > MS_DEST_MIDI) h->dest[t] = MS_DEST_INSTR;
        if ((h->in[t] & 15) > MS_DEST_MIDI) h->in[t] = MS_DEST_INSTR;
        h->ch[t] &= 15; h->mute[t] &= 1;
        for (int p = 0; p < MS_PAGES; p++) if (h->first[t][p] != MS_NONE && h->first[t][p] >= MS_SLOTS) h->first[t][p] = MS_NONE;
    }
    return 0;
}
int ms_load(ms_t *s)
{
    int r = s->env->read_head(&s->h, (int)sizeof s->h);
    if (!r && !head_check(&s->h)) { scan_start(s); return 0; }   /* (the scan frees slots leaked by damage) */
    const ms_env *e = s->env;
    ms_init(s, e);
    if (r < 0) { s->ro = 1; return -1; }     /* a read error is not damage: write nothing until ms_service reads it */
    return 1;
}

/* ---- slots and RAM buffers ---- */
static int slot_alloc(ms_t *s)
{
    if (s->ro) return -1;                   /* header unread: the used map is unknown */
    s->gen++;
    for (int i = 0; i < MS_SLOTS; i++)
        if (!(s->h.used[i >> 3] & (1 << (i & 7)))) { s->h.used[i >> 3] |= (uint8_t)(1 << (i & 7)); s->head_dirty = 1; return i; }
    return -1;
}
static void slot_free(ms_t *s, int slot)
{
    s->h.used[slot >> 3] &= (uint8_t)~(1 << (slot & 7)); s->head_dirty = 1; s->gen++;
    int b = s->where[slot];
    if (b != 0xFF) { s->ram_slot[b] = MS_NONE; s->ram_dirty[b] = 0; s->where[slot] = 0xFF; }
}
static int buf_free(ms_t *s)                /* a free RAM buffer, -1 if none (no I/O) */
{
    for (int i = 0; i < MS_RAM; i++) if (s->ram_slot[i] == MS_NONE) return i;
    return -1;
}
static ms_slot *res(ms_t *s, int slot)      /* resident slot or 0 */
{
    if (slot < 0 || slot >= MS_SLOTS) return 0;
    int b = s->where[slot];
    if (b == 0xFF) return 0;
    s->ram_use[b] = 1;
    return &s->ram[b];
}
static ms_slot *slot_new(ms_t *s, int track, int page)   /* new empty resident slot (not linked) */
{
    int b = buf_free(s), slot = b < 0 ? -1 : slot_alloc(s);
    if (slot < 0) return 0;
    ms_slot *x = &s->ram[b];
    clr(x, (int)sizeof *x);
    x->magic = MS_MAGIC; x->slot = (uint16_t)slot; x->track = (uint8_t)track; x->page = (uint16_t)page; x->next = MS_NONE;
    x->ver = 1;
    s->ram_slot[b] = (uint16_t)slot; s->where[slot] = (uint8_t)b; s->ram_dirty[b] = 1; s->ram_use[b] = 1;
    return x;
}
static void dirty(ms_t *s, ms_slot *x) { s->ram_dirty[x - s->ram] = 1; }

/* ---- events ---- */
static int insert(ms_t *s, int track, int32_t pos, const ms_ev *e0)   /* 0 ok; caller holds the interrupts off */
{
    if (pos < 0 || pos > MS_MAXPOS) return -1;
    int page = (int)(pos >> MS_PAGE_BITS);
    ms_ev e = *e0;
    e.off = (uint16_t)(pos & 0xFFFF);
    uint16_t *link = &s->h.first[track][page];
    if (*link == MS_NONE) {
        ms_slot *x = slot_new(s, track, page);
        if (!x) { s->dropped++; return -1; }
        *link = x->slot; s->head_dirty = 1;
    }
    ms_slot *x = res(s, *link);
    if (!x) { s->underruns++; s->dropped++; return -1; }
    for (int hop = 0;; hop++) {                 /* the slot whose range takes e: before the next slot's first event */
        if (hop >= MS_RAM) { s->dropped++; return -1; }   /* (more than every buffer: a damaged chain) */
        ms_slot *nx = x->next == MS_NONE ? 0 : res(s, x->next);
        if (x->next != MS_NONE && !nx) { s->underruns++; s->dropped++; return -1; }
        if (!nx || !nx->n || e.off < nx->ev[0].off) break;
        x = nx;
    }
    if (x->n >= MS_SLOT_EV) {                   /* split: upper half into a new slot after x */
        ms_slot *y = slot_new(s, track, page);
        if (!y) { s->dropped++; return -1; }
        int h = MS_SLOT_EV / 2;
        for (int i = h; i < x->n; i++) y->ev[i - h] = x->ev[i];
        y->n = (uint8_t)(x->n - h); x->n = (uint8_t)h;
        y->next = x->next; x->next = y->slot;
        dirty(s, x);
        if (e.off >= y->ev[0].off) x = y;
    }
    int i = x->n;
    while (i > 0 && x->ev[i - 1].off > e.off) { x->ev[i] = x->ev[i - 1]; i--; }   /* after equal offsets */
    x->ev[i] = e; x->n++;
    dirty(s, x);
    return 0;
}
static ms_ev *find_note(ms_t *s, int track, int32_t pos, int note, int ch)   /* the open note recorded at pos */
{
    if (pos < 0 || pos > MS_MAXPOS) return 0;
    uint16_t off = (uint16_t)(pos & 0xFFFF);
    for (int sl = s->h.first[track][pos >> MS_PAGE_BITS], hop = 0; sl != MS_NONE && hop < MS_RAM; hop++) {
        ms_slot *x = res(s, sl);
        if (!x) return 0;
        for (int i = x->n - 1; i >= 0; i--)
            if (x->ev[i].off == off && x->ev[i].kind == MS_NOTE && x->ev[i].d1 == note && x->ev[i].ch == ch &&
                !x->ev[i].len) { dirty(s, x); return &x->ev[i]; }
        sl = x->next;
    }
    return 0;
}

/* ---- playback ---- */
static void off_one(ms_t *s, int i)
{
    s->env->play(s->act[i].track, s->act[i].dest, s->act[i].ch, MS_NOTE, s->act[i].note, 0, 0);
    s->act[i] = s->act[--s->nact];
}
void ms_all_off(ms_t *s) { while (s->nact) off_one(s, s->nact - 1); }
static void emit(ms_t *s, int t, const ms_ev *e)
{
    int dest = s->h.dest[t], ch = e->ch;
    if (dest == MS_DEST_MIDI) ch = s->h.ch[t];
    if (e->kind == MS_CC) { s->env->play(t, dest, ch, MS_CC, e->d1, e->d2, 1); return; }
    for (int i = 0; i < s->nact; i++)                         /* retrigger: the sounding one ends first */
        if (s->act[i].track == t && s->act[i].note == e->d1) { off_one(s, i); break; }
    if (s->nact >= MS_ACTIVE) off_one(s, 0);
    s->env->play(t, dest, ch, MS_NOTE, e->d1, e->d2, 1);
    s->act[s->nact].track = (uint8_t)t; s->act[s->nact].note = e->d1; s->act[s->nact].dest = (uint8_t)dest;
    s->act[s->nact].ch = (uint8_t)ch; s->act[s->nact].left = e->len ? (int32_t)e->len * 64 : 64 * 8;
    s->nact++;
}
/* events of every unmuted track with position in (a, b] (forward) or [b, a) (reverse) */
static void cross(ms_t *s, int32_t a, int32_t b)
{
    int fwd = b > a;
    int32_t lo = fwd ? a + 1 : b, hi = fwd ? b : a - 1;
    if (lo < 0) lo = 0;
    if (hi > MS_MAXPOS) hi = MS_MAXPOS;
    if (hi < lo) return;
    for (int t = 0; t < MS_TRACKS; t++) {
        if (s->h.mute[t]) continue;
        for (int p = (int)(lo >> MS_PAGE_BITS); p <= (int)(hi >> MS_PAGE_BITS); p++)
            for (int sl = s->h.first[t][p], hop = 0; sl != MS_NONE && hop < MS_RAM; hop++) {   /* (bounded: audio thread) */
                ms_slot *x = res(s, sl);
                if (!x) { s->underruns++; break; }
                for (int i = 0; i < x->n; i++) {
                    int32_t q = ((int32_t)p << MS_PAGE_BITS) | x->ev[i].off;
                    if (q >= lo && q <= hi) emit(s, t, &x->ev[i]);
                }
                sl = x->next;
            }
    }
}
void ms_block(ms_t *s)
{
    const ms_env *e = s->env;
    int32_t pos = e->tape_pos();
    if (!e->tape_playing()) {
        if (s->prev >= 0) ms_all_off(s);
        s->prev = -1; s->nheld = 0;
        return;
    }
    if (s->prev < 0) s->prev = pos - 1;          /* exclusive: an event at the start position plays */
    int32_t d = pos - s->prev, ad = d < 0 ? -d : d;
    for (int i = 0; i < s->nact;) {             /* sounding notes: tape samples left */
        s->act[i].left -= ad;
        if (s->act[i].left <= 0) off_one(s, i); else i++;
    }
    int32_t in, out;
    if (ad <= MS_JUMP) cross(s, s->prev, pos);
    else {                                       /* loop wrap or locate */
        ms_all_off(s);
        if (e->loop(&in, &out) && out > in) {
            int fwd = d < 0;                     /* forward travel wraps back down to the loop start */
            cross(s, s->prev, fwd ? out : in);
            ms_all_off(s);
            cross(s, fwd ? in - 1 : in + 1, pos);
        }
    }
    s->prev = pos;
}

/* ---- recording (audio thread) ---- */
void ms_note_in(ms_t *s, int note, int vel, int ch, int on)
{
    if (!s->rec || s->prev < 0 || note < 0 || note > 127) return;
    int t = s->sel;
    if (on) {
        if (s->nheld >= MS_HELD) return;
        ms_ev e = { 0, MS_NOTE, (uint8_t)note, (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel), (uint8_t)(ch & 15), 0 };
        if (insert(s, t, s->prev, &e)) return;
        s->held[s->nheld].note = (uint8_t)note; s->held[s->nheld].ch = (uint8_t)(ch & 15); s->held[s->nheld].pos = s->prev;
        s->nheld++;
        if (s->ntake < MS_TAKE) {
            s->take[s->ntake].pos = s->prev; s->take[s->ntake].note = (uint8_t)note;
            s->take[s->ntake].ch = (uint8_t)(ch & 15); s->take[s->ntake].track = (uint8_t)t; s->ntake++;
        }
        return;
    }
    for (int i = 0; i < s->nheld; i++)
        if (s->held[i].note == note && s->held[i].ch == (ch & 15)) {
            int32_t len = s->prev - s->held[i].pos;
            if (len < 0) len = -len;
            ms_ev *ev = find_note(s, t, s->held[i].pos, note, ch & 15);
            if (ev) { len = (len + 63) / 64; ev->len = (uint16_t)(len < 1 ? 1 : len > 0xFFFF ? 0xFFFF : len); }
            s->held[i] = s->held[--s->nheld];
            return;
        }
}
void ms_cc_in(ms_t *s, int cc, int val, int ch)
{
    if (!s->rec || s->prev < 0 || cc < 0 || cc > 127) return;
    int32_t d = s->prev - s->cc_last[cc];
    if (d >= 0 && d < MS_CC_THIN) return;
    ms_ev e = { 0, MS_CC, (uint8_t)cc, (uint8_t)(val & 127), (uint8_t)(ch & 15), 0 };
    if (!insert(s, s->sel, s->prev, &e)) s->cc_last[cc] = s->prev;
}

/* ---- main loop: cache ---- */
static int load_slot(ms_t *s, int slot, int track, int page)   /* resident (reads it); -1 no buffer; 1 not of this chain */
{
    if (!(s->h.used[slot >> 3] & (1 << (slot & 7)))) return 1;   /* a free slot: stale link (a lost slot write) */
    if (s->where[slot] != 0xFF) { ms_slot *x = &s->ram[s->where[slot]]; return x->track != track || x->page != page; }
    int b = buf_free(s);
    if (b < 0) return -1;
    ms_slot *x = &s->ram[b];
    int r = s->env->read_slot(slot, x);
    if (r < 0) return -1;                       /* a read error is not damage: nothing written, the next prefetch retries */
    if (r || x->magic != MS_MAGIC || x->slot != slot || x->n > MS_SLOT_EV || slot_crc(x) != x->crc) {
        clr(x, (int)sizeof *x);                  /* damaged: an empty slot, the rest of the chain is lost */
        x->magic = MS_MAGIC; x->slot = (uint16_t)slot; x->track = (uint8_t)track; x->page = (uint16_t)page;
        x->next = MS_NONE; x->ver = 1;
        s->ram_dirty[b] = 1;
        scan_start(s);                           /* (its slots: free again) */
    } else if (x->track != track || x->page != page) return 1;   /* another chain's slot */
    else s->ram_dirty[b] = 0;
    if (x->next != MS_NONE && x->next >= MS_SLOTS) x->next = MS_NONE;
    uint32_t m = s->env->irq_off();
    s->ram_slot[b] = (uint16_t)slot; s->where[slot] = (uint8_t)b; s->ram_use[b] = 1;
    s->env->irq_on(m);
    return 0;
}
static int write_buf(ms_t *s, int b)
{
    static ms_slot tmp;
    uint32_t m = s->env->irq_off();
    cpy(&tmp, &s->ram[b], (int)sizeof tmp);
    s->ram_dirty[b] = 0;
    s->env->irq_on(m);
    tmp.crc = 0; tmp.crc = slot_crc(&tmp);
    if (s->env->write_slot(tmp.slot, &tmp)) { s->ram_dirty[b] = 1; return -1; }
    return 0;
}
static int in_window(ms_t *s, const ms_slot *x, int page, int lp)
{
    return (x->page + 1 >= page && x->page <= page + 3) || (lp >= 0 && x->page >= lp && x->page <= lp + 1);
}
static int in_chain(ms_t *s, int track, int page, int slot, int k)   /* slot among the first k of the chain */
{
    for (int sl = s->h.first[track][page]; k-- > 0; sl = s->ram[s->where[sl]].next) if (sl == slot) return 1;
    return 0;
}
int ms_load_range(ms_t *s, int track, int pa, int pb)   /* make pages [pa, pb] of a track resident; 0 all */
{
    int ok = 0;
    for (int p = pa < 0 ? 0 : pa; p <= pb && p < MS_PAGES; p++) {
        uint16_t *link = &s->h.first[track][p];
        ms_slot *px = 0;
        for (int k = 0; *link != MS_NONE; k++) {
            int r = in_chain(s, track, p, *link, k) ? 1 : load_slot(s, *link, track, p);
            if (r < 0) { ok = -1; break; }
            if (r) {                                /* stale, another chain's or a cycle (lost slot write): the chain ends */
                uint32_t m = s->env->irq_off();
                *link = MS_NONE;
                if (px) dirty(s, px); else s->head_dirty = 1;
                s->env->irq_on(m);
                break;
            }
            px = res(s, *link); link = &px->next;
        }
    }
    return ok;
}
/* rebuild the used map from the chains (after a load, after damage): slots no chain reaches are freed. A step per I/O
 * of budget, tape stopped; any slot allocated or freed meanwhile restarts it, a read error retries the same slot */
static void scan_step(ms_t *s, int budget)
{
    static ms_slot tmp;
    while (s->scan >= 0 && budget > 0) {
        if (s->scan_gen != s->gen) scan_start(s);
        if (s->scan >= MS_TRACKS * MS_PAGES) {
            for (int b = 0; b < MS_RAM; b++)                 /* (resident slots stay: a buffer holds them) */
                if (s->ram_slot[b] != MS_NONE) s->seen[s->ram_slot[b] >> 3] |= (uint8_t)(1 << (s->ram_slot[b] & 7));
            uint32_t m = s->env->irq_off();
            if (s->scan_gen == s->gen) {                      /* (else: restarts) */
                for (int i = 0; i < MS_SLOTS / 8; i++)
                    if (s->h.used[i] & ~s->seen[i]) { s->h.used[i] &= s->seen[i]; s->head_dirty = 1; }
                s->scan = -1;
            }
            s->env->irq_on(m);
            continue;
        }
        int t = s->scan / MS_PAGES, p = s->scan % MS_PAGES;
        int sl = s->scan_sl == MS_NONE ? s->h.first[t][p] : s->scan_sl;
        const ms_slot *x = 0;
        if (sl != MS_NONE && sl < MS_SLOTS && (s->h.used[sl >> 3] >> (sl & 7) & 1) && !(s->seen[sl >> 3] >> (sl & 7) & 1)) {
            if (s->where[sl] != 0xFF) x = &s->ram[s->where[sl]];
            else {
                int r = s->env->read_slot(sl, &tmp);
                budget--;
                if (r < 0) return;
                x = &tmp;
                if (r || tmp.magic != MS_MAGIC || tmp.slot != sl || tmp.n > MS_SLOT_EV || slot_crc(&tmp) != tmp.crc) {
                    s->seen[sl >> 3] |= (uint8_t)(1 << (sl & 7));   /* damaged: kept (loads as an empty slot), the chain ends */
                    x = 0;
                }
            }
            if (x && (x->track != t || x->page != p)) x = 0;  /* another chain's: counted there */
        }   /* (a free or seen slot: stale link, cycle; the chain ends) */
        if (x) { s->seen[sl >> 3] |= (uint8_t)(1 << (sl & 7)); s->scan_sl = x->next; }
        if (!x || x->next == MS_NONE) { s->scan++; s->scan_sl = MS_NONE; }
    }
}
void ms_service(ms_t *s, int budget)
{
    if (s->ro) {                                             /* header unread: retry, write nothing meanwhile */
        static ms_head tmp;
        int r = s->env->read_head(&tmp, (int)sizeof tmp);
        if (r < 0) return;
        if (!r && !head_check(&tmp)) {                       /* (else none or damaged: the empty song stays) */
            uint32_t m = s->env->irq_off();
            cpy(&s->h, &tmp, (int)sizeof tmp);
            s->env->irq_on(m);
            scan_start(s);
        }
        s->ro = 0;
    }
    int32_t pos = s->env->tape_pos();
    int page = pos < 0 ? 0 : (int)(pos >> MS_PAGE_BITS);
    int32_t lin, lout;                                       /* loop on: its start pages stay resident (the wrap) */
    int lp = s->env->loop(&lin, &lout) && lout > lin && lin >= 0 ? (int)(lin >> MS_PAGE_BITS) : -1;
    for (int b = 0; b < MS_RAM && budget > 0; b++)          /* write back dirty buffers */
        if (s->ram_slot[b] != MS_NONE && s->ram_dirty[b]) { write_buf(s, b); budget--; }
    int dirty_left = 0;
    for (int b = 0; b < MS_RAM; b++) dirty_left |= s->ram_slot[b] != MS_NONE && s->ram_dirty[b];
    if (s->head_dirty && !dirty_left && budget > 0) {       /* header after the slots it points to */
        static ms_head tmp;
        uint32_t m = s->env->irq_off();
        cpy(&tmp, &s->h, (int)sizeof tmp);
        s->head_dirty = 0;
        s->env->irq_on(m);
        tmp.crc = 0; tmp.crc = ms_crc(&tmp, (int)sizeof tmp);
        if (s->env->write_head(&tmp, (int)sizeof tmp)) s->head_dirty = 1;
        budget--;
    }
    int nfree = 0;                                           /* evict clean buffers outside the window */
    for (int b = 0; b < MS_RAM; b++) nfree += s->ram_slot[b] == MS_NONE;
    for (int b = 0; b < MS_RAM && nfree < MS_RESERVE + 16; b++) {
        if (s->ram_slot[b] == MS_NONE || s->ram_dirty[b] || in_window(s, &s->ram[b], page, lp)) continue;
        if (s->ram_use[b]) { s->ram_use[b] = 0; continue; }  /* second chance */
        uint32_t m = s->env->irq_off();
        s->where[s->ram_slot[b]] = 0xFF; s->ram_slot[b] = MS_NONE;
        s->env->irq_on(m);
        nfree++;
    }
    for (int t = 0; t < MS_TRACKS; t++) {                     /* prefetch: one page behind, three ahead */
        ms_load_range(s, t, page - 1, page + 3);
        if (lp >= 0) ms_load_range(s, t, lp, lp + 1);        /* + the loop start */
    }
    if (s->prev < 0 && !dirty_left) scan_step(s, budget);
}

/* ---- editing (main loop) ---- */
static int load_track_range(ms_t *s, int t, int32_t a, int32_t b)
{
    return ms_load_range(s, t, (int)(a >> MS_PAGE_BITS), (int)(b >> MS_PAGE_BITS));
}
/* copy the events starting in [a, b) of the tracks in mask (bit t) into the clipboard, positions relative to the
 * first copied event (clip_first: its position, -1 when nothing was copied); -1 and an empty clipboard when more
 * than MS_CLIP events */
int ms_copy(ms_t *s, int32_t a, int32_t b, int mask)
{
    if (a < 0) a = 0;
    if (b > MS_MAXPOS + 1) b = MS_MAXPOS + 1;
    if (b <= a) return -1;
    for (int t = 0; t < MS_TRACKS; t++) if ((mask >> t & 1) && load_track_range(s, t, a, b - 1)) return -1;
    s->nclip = 0; s->clip_first = -1; s->clip_all = mask != (1 << s->sel);
    for (int t = 0; t < MS_TRACKS; t++) {
        if (!(mask >> t & 1)) continue;
        for (int p = (int)(a >> MS_PAGE_BITS); p <= (int)((b - 1) >> MS_PAGE_BITS); p++)
            for (int sl = s->h.first[t][p], hop = 0; sl != MS_NONE && hop < MS_RAM; hop++) {
                ms_slot *x = res(s, sl);
                for (int i = 0; i < x->n; i++) {
                    int32_t q = ((int32_t)p << MS_PAGE_BITS) | x->ev[i].off;
                    if (q < a || q >= b) continue;
                    if (s->nclip >= MS_CLIP) { s->nclip = 0; s->clip_len = s->clip_first = -1; return -1; }   /* (a cut would lose the rest) */
                    s->clip[s->nclip] = x->ev[i]; s->clip_pos[s->nclip] = q; s->clip_trk[s->nclip++] = (uint8_t)t;
                    if (s->clip_first < 0 || q < s->clip_first) s->clip_first = q;
                }
                sl = x->next;
            }
    }
    for (int i = 0; i < s->nclip; i++) s->clip_pos[i] -= s->clip_first;
    s->clip_len = s->nclip ? b - s->clip_first : -1;
    return 0;
}
int ms_cut(ms_t *s, int32_t a, int32_t b, int mask)
{
    if (ms_copy(s, a, b, mask)) return -1;
    for (int t = 0; t < MS_TRACKS; t++) {
        if (!(mask >> t & 1)) continue;
        for (int p = (int)(a >> MS_PAGE_BITS); p <= (int)((b - 1) >> MS_PAGE_BITS); p++)
            for (int sl = s->h.first[t][p], hop = 0; sl != MS_NONE && hop < MS_RAM; hop++) {
                ms_slot *x = res(s, sl);
                uint32_t m = s->env->irq_off();
                int o = 0;
                for (int i = 0; i < x->n; i++) {
                    int32_t q = ((int32_t)p << MS_PAGE_BITS) | x->ev[i].off;
                    if (q < a || q >= b) x->ev[o++] = x->ev[i];
                }
                if (o != x->n) { x->n = (uint8_t)o; dirty(s, x); }
                s->env->irq_on(m);
                sl = x->next;
            }
        for (int p = (int)(a >> MS_PAGE_BITS); p <= (int)((b - 1) >> MS_PAGE_BITS); p++) {   /* drop empty slots */
            uint32_t m = s->env->irq_off();
            uint16_t *link = &s->h.first[t][p];
            for (int hop = 0; *link != MS_NONE && hop < MS_RAM; hop++) {
                ms_slot *x = res(s, *link);
                if (!x) break;
                if (!x->n) { int sl = *link; *link = x->next; slot_free(s, sl); s->head_dirty = 1; }
                else link = &x->next;
            }
            s->env->irq_on(m);
        }
    }
    ms_all_off(s);
    return 0;
}
/* paste with the first event at `at`: into the tracks it came from (all-track lift), else into the selected track */
int ms_paste(ms_t *s, int32_t at)
{
    if (s->clip_len < 0 || at < 0 || at + s->clip_len > MS_MAXPOS + 1) return -1;
    for (int t = 0; t < MS_TRACKS; t++)
        if ((s->clip_all || t == s->sel) && load_track_range(s, t, at, at + s->clip_len)) return -1;
    int bad = 0;
    for (int i = 0; i < s->nclip; i++) {
        uint32_t m = s->env->irq_off();
        bad |= insert(s, s->clip_all ? s->clip_trk[i] : s->sel, at + s->clip_pos[i], &s->clip[i]);
        s->env->irq_on(m);
    }
    return bad;
}

/* ---- quantize (main loop) ---- */
int32_t ms_grid_step(int grid, int32_t bpm_q8)
{
    static const uint8_t div[MS_Q_N] = { 0, 1, 2, 4, 8, 3, 6 };          /* steps per beat */
    if (grid <= MS_Q_OFF || grid >= MS_Q_N || bpm_q8 <= 0) return 0;
    return (int32_t)(((int64_t)2646000 << 8) / ((int64_t)bpm_q8 * div[grid]));   /* 44100 * 60 / bpm / div */
}
static int32_t snap(int32_t p, int32_t step) { int32_t q = (p + step / 2) / step * step; return q > MS_MAXPOS ? q - step : q; }
/* the first note (track t, position pos, note / channel) of its chain: its slot (0: none), index, link to the slot */
static ms_slot *note_at(ms_t *s, int t, int32_t pos, int note, int ch, uint16_t **lk, int *ix)
{
    uint16_t off = (uint16_t)(pos & 0xFFFF);
    uint16_t *link = &s->h.first[t][pos >> MS_PAGE_BITS];
    for (int hop = 0; *link != MS_NONE && hop < MS_RAM; hop++) {
        ms_slot *x = res(s, *link);
        if (!x) return 0;
        for (int i = 0; i < x->n; i++)
            if (x->ev[i].off == off && x->ev[i].kind == MS_NOTE && x->ev[i].d1 == note && x->ev[i].ch == ch) { *lk = link; *ix = i; return x; }
        link = &x->next;
    }
    return 0;
}
/* move one note (track t, position pos, note / channel) to np: inserted there (len kept), then removed from its slot */
static int move_note(ms_t *s, int t, int32_t pos, int note, int ch, int32_t np)
{
    if (np == pos) return 0;
    if (ms_load_range(s, t, (int)(pos >> MS_PAGE_BITS), (int)(pos >> MS_PAGE_BITS)) ||
        ms_load_range(s, t, (int)(np >> MS_PAGE_BITS), (int)(np >> MS_PAGE_BITS))) return -1;
    uint16_t *link; int i;
    uint32_t m = s->env->irq_off();
    ms_slot *x = note_at(s, t, pos, note, ch, &link, &i);    /* (0: edited away meanwhile) */
    int r = x ? insert(s, t, np, &x->ev[i]) : -1;          /* the copy first: a failed insert loses nothing */
    if (!r && (x = note_at(s, t, pos, note, ch, &link, &i))) {   /* again: the insert may have split or shifted it */
        for (int j = i; j + 1 < x->n; j++) x->ev[j] = x->ev[j + 1];
        x->n--; dirty(s, x);
        if (!x->n) { int sl = *link; *link = x->next; slot_free(s, sl); s->head_dirty = 1; }
    }
    s->env->irq_on(m);
    return r;
}
int ms_quantize(ms_t *s, int track, int32_t a, int32_t b, int32_t step)
{
    if (step <= 0 || b <= a) return 0;
    if (a < 0) a = 0;
    if (b > MS_MAXPOS + 1) b = MS_MAXPOS + 1;
    if (ms_load_range(s, track, (int)(a >> MS_PAGE_BITS), (int)((b - 1) >> MS_PAGE_BITS))) return -1;
    static ms_ev ev[MS_TAKE]; static int32_t ps[MS_TAKE];
    int n = 0;                                    /* collect first: moved notes must not be met again */
    for (int p = (int)(a >> MS_PAGE_BITS); p <= (int)((b - 1) >> MS_PAGE_BITS) && n < MS_TAKE; p++)
        for (int sl = s->h.first[track][p], hop = 0; sl != MS_NONE && n < MS_TAKE && hop < MS_RAM; hop++) {
            ms_slot *x = res(s, sl);
            if (!x) break;
            for (int i = 0; i < x->n && n < MS_TAKE; i++) {
                int32_t q = ((int32_t)p << MS_PAGE_BITS) | x->ev[i].off;
                if (q >= a && q < b && x->ev[i].kind == MS_NOTE) { ev[n] = x->ev[i]; ps[n++] = q; }
            }
            sl = x->next;
        }
    int moved = 0;
    for (int i = 0; i < n; i++) if (!move_note(s, track, ps[i], ev[i].d1, ev[i].ch, snap(ps[i], step))) moved++;
    return moved;
}
int ms_quantize_take(ms_t *s, int32_t step)
{
    int moved = 0;
    for (int i = 0; step > 0 && i < s->ntake; i++)
        if (!move_note(s, s->take[i].track, s->take[i].pos, s->take[i].note, s->take[i].ch, snap(s->take[i].pos, step))) moved++;
    s->ntake = 0;
    return moved;
}
void ms_take_reset(ms_t *s) { s->ntake = 0; }

/* ---- queries ---- */
int ms_events(ms_t *s, int t, int32_t a, int32_t b, ms_ev *out, int32_t *pos, int max)
{
    int n = 0;
    if (a < 0) a = 0;
    if (b > MS_MAXPOS) b = MS_MAXPOS;
    for (int p = (int)(a >> MS_PAGE_BITS); p <= (int)(b >> MS_PAGE_BITS) && n < max; p++)
        for (int sl = s->h.first[t][p], hop = 0; sl != MS_NONE && n < max && hop < MS_RAM; hop++) {   /* (bounded: GUI thread) */
            ms_slot *x = res(s, sl);
            if (!x) break;
            for (int i = 0; i < x->n && n < max; i++) {
                int32_t q = ((int32_t)p << MS_PAGE_BITS) | x->ev[i].off;
                if (q >= a && q <= b) { if (out) out[n] = x->ev[i]; if (pos) pos[n] = q; n++; }
            }
            sl = x->next;
        }
    return n;
}
int ms_count(ms_t *s, int t) { return ms_events(s, t, 0, MS_MAXPOS, 0, 0, 1 << 30); }
