/* Development update over SysEx: see du.h and docs/35. */
#include "du.h"

static const uint8_t du_hdr[DU_SYSEX_HDR] = { 0xF0, 0x00, 0x20, 0x76, 0x7D };

uint32_t du_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int du_pack7(const uint8_t *in, int n, uint8_t *out)
{
    int o = 0;
    for (int i = 0; i < n; i += 7) {
        int m = n - i < 7 ? n - i : 7;
        uint8_t hi = 0;
        for (int k = 0; k < m; k++) hi |= (uint8_t)((in[i + k] >> 7) << k);
        out[o++] = hi;
        for (int k = 0; k < m; k++) out[o++] = in[i + k] & 0x7F;
    }
    return o;
}

int du_unpack7(const uint8_t *in, int n, uint8_t *out)
{
    int o = 0;
    for (int i = 0; i < n;) {
        uint8_t hi = in[i++];
        if (hi & 0x80) return -1;
        for (int k = 0; k < 7 && i < n; k++, i++) {
            if (in[i] & 0x80) return -1;
            out[o++] = (uint8_t)(in[i] | ((hi >> k & 1) << 7));
        }
    }
    return o;
}

int du_build(uint8_t *msg, int cmd, int seq, const uint8_t *payload, int n)
{
    if (DU_SYSEX_HDR + 3 + (n + 6) / 7 * 8 + 1 > DU_MAX_MSG) return -1;
    int o = 0;
    for (int i = 0; i < DU_SYSEX_HDR; i++) msg[o++] = du_hdr[i];
    msg[o++] = (uint8_t)(cmd & 0x7F);
    msg[o++] = (uint8_t)(seq & 0x7F);
    msg[o++] = (uint8_t)(seq >> 7 & 0x7F);
    o += du_pack7(payload, n, msg + o);
    msg[o++] = 0xF7;
    return o;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int contains_ci(const char *s, const char *w)
{
    for (; *s; s++) {
        int i = 0;
        while (w[i] && lower((unsigned char)s[i]) == w[i]) i++;
        if (!w[i]) return 1;
    }
    return 0;
}

static int du_plugin_path(const char *p, int n)          /* plugins/<name>.op1p, no subfolder (docs/37) */
{
    static const char pre[] = "plugins/", suf[] = ".op1p";
    for (int i = 0; i < 8; i++) if (p[i] != pre[i]) return 0;
    for (int i = 8; i < n; i++) if (p[i] == '/') return 0;
    if (n < 8 + 6) return 0;
    for (int i = 0; i < 5; i++) if (p[n - 5 + i] != suf[i]) return 0;
    return 1;
}

int du_plugin_file(const char *p) { int n = 0; while (p[n]) n++; return du_path_ok(p) && du_plugin_path(p, n); }

int du_path_ok(const char *p)
{
    int n = 0;
    for (const char *q = p; *q; q++, n++) {
        char c = *q;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-' || c == '/')) return 0;
    }
    if (n == 0 || n >= DU_MAX_PATH || p[0] == '/' || p[n - 1] == '/') return 0;
    if (contains_ci(p, "te-boot") || contains_ci(p, "te_boot") || contains_ci(p, "teboot")) return 0;   /* never */
    if (contains_ci(p, "..") || contains_ci(p, "//")) return 0;
    if (!(p[0] == 'O' && p[1] == 'P' && p[2] == '1' && p[3] == '_' && p[4] == 'v' && p[5] == 'd' && p[6] == 'k' &&
          p[7] == '.' && p[8] == 'l' && p[9] == 'd' && p[10] == 'r' && p[11] == 0) &&
        !(p[0] == 'c' && p[1] == 'o' && p[2] == 'n' && p[3] == 't' && p[4] == 'e' && p[5] == 'n' && p[6] == 't' &&
          p[7] == '/') && !du_plugin_path(p, n))
        return 0;
    return 1;
}

void du_stage_name(int i, char *out)
{
    const char *d = DU_STAGE_DIR "/f";
    int o = 0;
    while (*d) out[o++] = *d++;
    out[o++] = (char)('0' + i / 100 % 10); out[o++] = (char)('0' + i / 10 % 10); out[o++] = (char)('0' + i % 10); out[o] = 0;
}

char du_text[DU_TEXT_CAP];

void du_init(du_session *s, const du_fs *fs)
{
    s->fs = fs; s->open = 0; s->written = s->crc = 0; s->n = 0;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int reply(uint8_t *rep, int cmd, int seq, int status, const uint8_t *pl, int n)
{
    uint8_t b[DU_MAX_REPLY];
    if (n > DU_MAX_REPLY - 1) n = DU_MAX_REPLY - 1;
    b[0] = (uint8_t)status;
    for (int i = 0; i < n; i++) b[1 + i] = pl[i];
    return du_build(rep, cmd | DU_REPLY, seq, b, 1 + n);
}

static void abort_session(du_session *s)
{
    if (s->open) s->fs->close(s->fs->ctx);
    char name[40];
    for (int i = 0; i <= DU_MAX_FILES; i++) { du_stage_name(i, name); s->fs->remove(s->fs->ctx, name); }   /* whole staging dir */
    s->fs->remove(s->fs->ctx, DU_MANIFEST);
    s->open = 0; s->n = 0;
}

void du_abort(du_session *s) { abort_session(s); }

int du_handle(du_session *s, const uint8_t *msg, int n, uint8_t *rep)
{
    if (n < DU_SYSEX_HDR + 4 || n > DU_MAX_MSG || msg[n - 1] != 0xF7) return 0;
    for (int i = 0; i < DU_SYSEX_HDR; i++) if (msg[i] != du_hdr[i]) return 0;
    int cmd = msg[5], seq = msg[6] | msg[7] << 7;
    uint8_t pl[DU_MAX_MSG];
    int pn = du_unpack7(msg + 8, n - 9, pl);
    if (pn < 0) return reply(rep, cmd, seq, DU_E_FORMAT, 0, 0);
    uint8_t out[DU_MAX_REPLY];
    switch (cmd) {
    case DU_HELLO:
        wr32(out, DU_PROTOCOL);
        return reply(rep, cmd, seq, DU_OK, out, 4);
    case DU_BEGIN: {                                              /* u32 size, u32 crc, path (0-terminated) */
        if (s->open) return reply(rep, cmd, seq, DU_E_STATE, 0, 0);
        if (pn < 10 || pl[pn - 1] != 0) return reply(rep, cmd, seq, DU_E_FORMAT, 0, 0);
        const char *path = (const char *)pl + 8;
        if (!du_path_ok(path)) return reply(rep, cmd, seq, DU_E_PATH, 0, 0);
        uint32_t size = rd32(pl);
        if (size > 127u * DU_BLOCK) return reply(rep, cmd, seq, DU_E_SIZE, 0, 0);
        if (s->n >= DU_MAX_FILES) return reply(rep, cmd, seq, DU_E_FULL, 0, 0);
        for (int i = 0; i < s->n; i++) {                          /* same path twice: refuse */
            const char *a = s->f[i].path, *b = path;
            while (*a && *a == *b) a++, b++;
            if (*a == *b) return reply(rep, cmd, seq, DU_E_STATE, 0, 0);
        }
        du_file *f = &s->f[s->n];
        int k = 0; while (path[k]) { f->path[k] = path[k]; k++; } f->path[k] = 0;
        f->size = size; f->crc = rd32(pl + 4);
        char name[40]; du_stage_name(s->n, name);
        if (s->fs->open_w(s->fs->ctx, name)) return reply(rep, cmd, seq, DU_E_IO, 0, 0);
        s->open = 1; s->written = 0; s->crc = 0;
        return reply(rep, cmd, seq, DU_OK, 0, 0);
    }
    case DU_DATA: {                                               /* u32 offset, data */
        if (!s->open) return reply(rep, cmd, seq, DU_E_STATE, 0, 0);
        if (pn < 4 || pn > 4 + DU_MAX_DATA) return reply(rep, cmd, seq, DU_E_FORMAT, 0, 0);
        uint32_t off = rd32(pl), len = (uint32_t)pn - 4;
        wr32(out, s->written);
        if (off != s->written) return reply(rep, cmd, seq, DU_E_OFFSET, out, 4);   /* host resends from there */
        if (s->written + len > s->f[s->n].size) return reply(rep, cmd, seq, DU_E_SIZE, out, 4);
        if (s->fs->write(s->fs->ctx, pl + 4, len)) { abort_session(s); return reply(rep, cmd, seq, DU_E_IO, 0, 0); }
        s->written += len; s->crc = du_crc32(s->crc, pl + 4, len);
        wr32(out, s->written);
        return reply(rep, cmd, seq, DU_OK, out, 4);
    }
    case DU_END: {
        if (!s->open) return reply(rep, cmd, seq, DU_E_STATE, 0, 0);
        s->fs->close(s->fs->ctx); s->open = 0;
        du_file *f = &s->f[s->n];
        if (s->written != f->size) { char name[40]; du_stage_name(s->n, name); s->fs->remove(s->fs->ctx, name); return reply(rep, cmd, seq, DU_E_SIZE, 0, 0); }
        if (s->crc != f->crc) { char name[40]; du_stage_name(s->n, name); s->fs->remove(s->fs->ctx, name); return reply(rep, cmd, seq, DU_E_CRC, 0, 0); }
        s->n++;
        return reply(rep, cmd, seq, DU_OK, 0, 0);
    }
    case DU_COMMIT: {
        if (s->open || s->n == 0) return reply(rep, cmd, seq, DU_E_STATE, 0, 0);
        if (!s->fs->key_pressed(s->fs->ctx)) return reply(rep, cmd, seq, DU_WAIT_KEY, 0, 0);   /* host asks again */
        int len = du_manifest_write(s, du_text, (int)sizeof du_text);
        if (len < 0 || s->fs->open_w(s->fs->ctx, DU_MANIFEST) || s->fs->write(s->fs->ctx, (const uint8_t *)du_text, (uint32_t)len)) {
            abort_session(s); return reply(rep, cmd, seq, DU_E_IO, 0, 0);
        }
        s->fs->close(s->fs->ctx);
        int r = reply(rep, cmd, seq, DU_OK, 0, 0);
        s->n = 0;
        s->fs->reboot(s->fs->ctx);                                /* (firmware: after the reply went out) */
        return r;
    }
    case DU_DELETE: {                                             /* path: staged delete (no data) */
        if (s->open) return reply(rep, cmd, seq, DU_E_STATE, 0, 0);
        if (pn < 2 || pl[pn - 1] != 0) return reply(rep, cmd, seq, DU_E_FORMAT, 0, 0);
        const char *path = (const char *)pl;
        if (!du_plugin_file(path)) return reply(rep, cmd, seq, DU_E_PATH, 0, 0);
        if (s->n >= DU_MAX_FILES) return reply(rep, cmd, seq, DU_E_FULL, 0, 0);
        for (int i = 0; i < s->n; i++) {
            const char *a = s->f[i].path, *b = path;
            while (*a && *a == *b) a++, b++;
            if (*a == *b) return reply(rep, cmd, seq, DU_E_STATE, 0, 0);
        }
        du_file *f = &s->f[s->n++];
        int k = 0; while (path[k]) { f->path[k] = path[k]; k++; } f->path[k] = 0;
        f->size = DU_DEL_SIZE; f->crc = 0;
        return reply(rep, cmd, seq, DU_OK, 0, 0);
    }
    case DU_LIST: {                                               /* u16 index */
        if (pn != 2) return reply(rep, cmd, seq, DU_E_FORMAT, 0, 0);
        uint32_t size;
        if (!s->fs->list || s->fs->list(s->fs->ctx, pl[0] | pl[1] << 8, (char *)out + 4, DU_MAX_REPLY - 6, &size))
            return reply(rep, cmd, seq, DU_E_NOFILE, 0, 0);
        wr32(out, size);
        int k = 0; while (k < DU_MAX_REPLY - 6 && out[4 + k]) k++;
        return reply(rep, cmd, seq, DU_OK, out, 4 + k);
    }
    case DU_STAT: {                                               /* path (0-terminated) */
        if (pn < 2 || pl[pn - 1] != 0) return reply(rep, cmd, seq, DU_E_FORMAT, 0, 0);
        if (!du_path_ok((const char *)pl)) return reply(rep, cmd, seq, DU_E_PATH, 0, 0);
        uint32_t size;
        if (!s->fs->stat || s->fs->stat(s->fs->ctx, (const char *)pl, &size)) return reply(rep, cmd, seq, DU_E_NOFILE, 0, 0);
        wr32(out, size);
        return reply(rep, cmd, seq, DU_OK, out, 4);
    }
    case DU_CRC: {                                                /* path (0-terminated), u32 offset, u32 crc */
        if (pn < 10 || pl[pn - 9] != 0) return reply(rep, cmd, seq, DU_E_FORMAT, 0, 0);
        const char *path = (const char *)pl;
        int k = 0; while (pl[k]) k++;
        if (k == 0 || k != pn - 9) return reply(rep, cmd, seq, DU_E_FORMAT, 0, 0);
        if (!du_path_ok(path)) return reply(rep, cmd, seq, DU_E_PATH, 0, 0);
        if (!s->fs->read) return reply(rep, cmd, seq, DU_E_STATE, 0, 0);
        uint32_t size;
        if (!s->fs->stat || s->fs->stat(s->fs->ctx, path, &size)) return reply(rep, cmd, seq, DU_E_NOFILE, 0, 0);
        uint32_t off = rd32(pl + k + 1), crc = rd32(pl + k + 5);
        if (off > size) return reply(rep, cmd, seq, DU_E_OFFSET, 0, 0);
        for (uint32_t end = size - off > DU_CRC_STEP ? off + DU_CRC_STEP : size; off < end;) {
            uint32_t n = end - off > sizeof du_text ? (uint32_t)sizeof du_text : end - off;   /* du_text: free outside COMMIT */
            int got = s->fs->read(s->fs->ctx, path, off, (uint8_t *)du_text, n);
            if (got <= 0 || (uint32_t)got > n) return reply(rep, cmd, seq, DU_E_IO, 0, 0);   /* shorter now: changed */
            crc = du_crc32(crc, (const uint8_t *)du_text, (uint32_t)got); off += (uint32_t)got;
        }
        wr32(out, crc); wr32(out + 4, off);
        return reply(rep, cmd, seq, DU_OK, out, 8);
    }
    case DU_IDENT: {
        int k = s->fs->ident ? s->fs->ident(s->fs->ctx, (char *)out, DU_MAX_REPLY - 1) : 0;
        return reply(rep, cmd, seq, DU_OK, out, k < 0 ? 0 : k);
    }
    case DU_ABORT:
        abort_session(s);
        return reply(rep, cmd, seq, DU_OK, 0, 0);
    default:
        return reply(rep, cmd, seq, DU_E_FORMAT, 0, 0);
    }
}

/* manifest: "op1r-stage 1\n", per file "F <size> <crc hex8> <path>\n", "N <count> <crc hex8 of the lines above>\n" */
static int put_s(char *o, int at, int cap, const char *s) { while (*s) { if (at >= cap - 1) return -1; o[at++] = *s++; } return at; }
static int put_u(char *o, int at, int cap, uint32_t v)
{
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) { if (at >= cap - 1) return -1; o[at++] = t[--n]; }
    return at;
}
static int put_x(char *o, int at, int cap, uint32_t v)
{
    for (int i = 7; i >= 0; i--) { if (at >= cap - 1) return -1; o[at++] = "0123456789abcdef"[v >> (i * 4) & 15]; }
    return at;
}

int du_manifest_write(const du_session *s, char *o, int cap)
{
    int at = put_s(o, 0, cap, "op1r-stage 1\n");
    for (int i = 0; i < s->n && at >= 0; i++) {
        at = put_s(o, at, cap, "F "); if (at < 0) break;
        at = put_u(o, at, cap, s->f[i].size); if (at < 0) break;
        at = put_s(o, at, cap, " "); if (at < 0) break;
        at = put_x(o, at, cap, s->f[i].crc); if (at < 0) break;
        at = put_s(o, at, cap, " "); if (at < 0) break;
        at = put_s(o, at, cap, s->f[i].path); if (at < 0) break;
        at = put_s(o, at, cap, "\n");
    }
    if (at < 0) return -1;
    uint32_t c = du_crc32(0, (const uint8_t *)o, (uint32_t)at);
    at = put_s(o, at, cap, "N "); if (at < 0) return -1;
    at = put_u(o, at, cap, (uint32_t)s->n); if (at < 0) return -1;
    at = put_s(o, at, cap, " "); if (at < 0) return -1;
    at = put_x(o, at, cap, c); if (at < 0) return -1;
    at = put_s(o, at, cap, "\n"); if (at < 0) return -1;
    o[at] = 0;
    return at;
}

static const char *get_u(const char *p, uint32_t *v)
{
    if (*p < '0' || *p > '9') return 0;
    uint32_t x = 0;
    while (*p >= '0' && *p <= '9') { if (x > 429496729u) return 0; x = x * 10 + (uint32_t)(*p++ - '0'); }
    *v = x; return p;
}
static const char *get_x(const char *p, uint32_t *v)
{
    uint32_t x = 0;
    for (int i = 0; i < 8; i++, p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
        if (d < 0) return 0;
        x = x << 4 | (uint32_t)d;
    }
    *v = x; return p;
}

int du_manifest_parse(const char *t, du_file *f, int *n)
{
    const char *p = t, *h = "op1r-stage 1\n";
    while (*h) if (*p++ != *h++) return -1;
    int k = 0;
    while (*p == 'F') {
        if (k >= DU_MAX_FILES || p[1] != ' ') return -1;
        p += 2;
        if (!(p = get_u(p, &f[k].size)) || *p++ != ' ' || !(p = get_x(p, &f[k].crc)) || *p++ != ' ') return -1;
        int i = 0;
        while (*p && *p != '\n') { if (i >= DU_MAX_PATH - 1) return -1; f[k].path[i++] = *p++; }
        f[k].path[i] = 0;
        if (*p++ != '\n' || !du_path_ok(f[k].path)) return -1;
        if (f[k].size == DU_DEL_SIZE ? !du_plugin_file(f[k].path) : f[k].size > 127u * DU_BLOCK) return -1;
        k++;
    }
    uint32_t c = du_crc32(0, (const uint8_t *)t, (uint32_t)(p - t)), cnt, cc;
    if (*p++ != 'N' || *p++ != ' ' || !(p = get_u(p, &cnt)) || *p++ != ' ' || !(p = get_x(p, &cc)) || *p++ != '\n' || *p)
        return -1;
    if (cnt != (uint32_t)k || cc != c || k == 0) return -1;
    *n = k;
    return 0;
}

/* ---- guarded flasher ---- */
int du_guard(int block) { return block >= DU_FIRST_BLOCK && block <= DU_LAST_BLOCK; }

int du_ldr_ok(const uint8_t *l, uint32_t size)
{
    if (size < 16 || size > 127u * DU_BLOCK || l[3] != 0xAD) return 0;      /* block code signature */
    uint8_t x = 0;
    for (int i = 0; i < 16; i++) x ^= l[i];
    return x == 0;                                                          /* header XOR checksum */
}

static int g_erase(const du_nand *d, int b) { return du_guard(b) ? d->erase(d->ctx, b) : -1; }
static int g_write(const du_nand *d, int b, const uint8_t *p) { return du_guard(b) ? d->write(d->ctx, b, p) : -1; }
static int g_read(const du_nand *d, int b, uint8_t *p) { return du_guard(b) ? d->read(d->ctx, b, p) : -1; }

int du_flash_ldr(const du_nand *d, const uint8_t *ldr, uint32_t size, uint8_t *buf)
{
    if (!du_ldr_ok(ldr, size)) return DU_F_BADLDR;
    /* blocks needed: the data, plus one 0xFF block if the last one is (almost) full (as te-boot's flasher) */
    int nb = (int)((size + DU_BLOCK - 1) / DU_BLOCK);
    uint32_t tail = size - (uint32_t)(nb - 1) * DU_BLOCK;
    int need = nb + (DU_BLOCK - tail < 0x100 ? 1 : 0);
    /* plan: the good blocks from DU_FIRST_BLOCK, before touching anything */
    int plan[DU_LAST_BLOCK + 1], np = 0;
    for (int b = DU_FIRST_BLOCK; b <= DU_LAST_BLOCK && np < need; b++)
        if (!d->is_bad(d->ctx, b)) plan[np++] = b;
    if (np < need) return DU_F_NOSPACE;
    for (int i = 0; i < np; i++) if (!du_guard(plan[i])) return DU_F_GUARD;
    /* erase every good block of the boot area from 4 (as te-boot), then write and verify the planned ones */
    for (int b = DU_FIRST_BLOCK; b <= DU_LAST_BLOCK; b++)
        if (!d->is_bad(d->ctx, b) && g_erase(d, b)) return DU_F_ERASE;
    for (int i = 0; i < need; i++) {
        const uint8_t *src = i < nb ? ldr + (uint32_t)i * DU_BLOCK : 0;
        uint32_t len = i < nb - 1 ? DU_BLOCK : i == nb - 1 ? tail : 0;
        for (uint32_t k = 0; k < DU_BLOCK; k++) buf[k] = k < len ? src[k] : 0xFF;
        if (g_write(d, plan[i], buf)) return DU_F_WRITE;
    }
    for (int i = 0; i < need; i++) {                                         /* read back everything */
        uint32_t len = i < nb - 1 ? DU_BLOCK : i == nb - 1 ? tail : 0;
        if (g_read(d, plan[i], buf)) return DU_F_VERIFY;
        for (uint32_t k = 0; k < DU_BLOCK; k++)
            if (buf[k] != (k < len ? ldr[(uint32_t)i * DU_BLOCK + k] : 0xFF)) return DU_F_VERIFY;
    }
    return DU_F_OK;
}
