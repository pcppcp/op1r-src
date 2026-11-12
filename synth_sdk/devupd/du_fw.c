/* Development update, firmware side: apply phase at boot (docs/35). Built always, hooked only with
 * make_firmware.py --dev: the boot's CALL access("/yaffs2/te-boot.op1") (0x019F01B8, after mount_yaffs, before USB /
 * audio / GUI = single-threaded) goes to du_boot_access, which applies a staged update first.
 * Apply: manifest valid -> every staged file's size + CRC checked (any failure: staging removed, nothing applied)
 * -> content files copied to their final paths -> OP1_vdk.ldr flashed to the boot area from block 4 with the stock
 * primitives in the stock order (erase 4..131 = 0x019EABB4, write_boot_block per block), every block number passing
 * du_guard (logical and physical after the bad-block mapping) -> staging removed -> "restart" (as the stock updater).
 * Never called: 0x019EAC08 (erases te-boot), flash_te_boot; paths with te-boot are refused by du_path_ok. */
#include "du.h"
#include "../screens/slotown.h"

#define Y_OPEN    ((int (*)(const char *, int, int))0x01A997F4)
#define Y_CLOSE   ((int (*)(int))0x01A99908)
#define Y_READ    ((int (*)(int, void *, unsigned))0x01A99C90)
#define Y_WRITE   ((int (*)(int, const void *, unsigned))0x01A99F84)
#define Y_UNLINK  ((int (*)(const char *))0x01A9A3FC)
#define Y_LSEEK   ((int (*)(int, int, int))0x01A9A0E4)        /* yaffs_lseek(fd, off, whence); 0 = SEEK_SET, 2 = SEEK_END -> size */
#define Y_ACCESS  ((int (*)(const char *, int))0x01A9A6B0)        /* 0 = exists */
#define Y_MKDIR   ((int (*)(const char *, int))0x01A9A886)   /* entry: P0 = R0 (path); 0x01A9A888 skips it */
#define O_READ    0
#define O_CREATE  0x241                                           /* O_WRONLY | O_CREAT | O_TRUNC (as the firmware) */
#define NEW_ARR   ((void *(*)(unsigned))0x01AA370C)
#define DEL_ARR   ((void (*)(void *))0x01AA2B04)
#define CONSOLE   ((void (*)(const char *))0x019D2BF0)            /* boot console line (the stock updater's) */
#define NAND_WAIT ((void (*)(void))0x01A5E454)                    /* as flash_firmware 0x019A1B48 */
#define NAND_LOCK ((void (*)(int))0x019E9DE4)
#define NAND_ERASE_MAIN ((void (*)(void))0x019EABB4)              /* erases blocks 4..131 only (checked) */
#define NAND_WRITE_BLOCK ((int (*)(int, const void *))0x019EAAAC) /* logical block, 128 KB; 0 ok */
#define UNMOUNT   ((void (*)(void))0x019A5F20)                    /* unmount_yaffs: as the stock end of a te-boot update */
#define DELAY_MS  ((void (*)(int))0x0193E5C0)
#define WDOG_CTL  (*(volatile uint16_t *)0xFFC00200)
#define WDOG_CNT  (*(volatile uint32_t *)0xFFC00204)
#define BAD_COUNT (*(volatile int *)0xFF907D94)
#define BAD_LIST  ((volatile int *)0x01DD94CC)

static int mapped(int logical)                                    /* write_boot_block's logical -> physical */
{
    int p = logical, n = BAD_COUNT;
    for (int i = 0; i < n && i < 128; i++) if (BAD_LIST[i] <= p) p++;
    return p;
}

int du_fw_flash(const uint8_t *ldr, uint32_t size)
{
    if (!du_ldr_ok(ldr, size)) return DU_F_BADLDR;
    int nb = (int)((size + DU_BLOCK - 1) / DU_BLOCK);
    uint32_t tail = size - (uint32_t)(nb - 1) * DU_BLOCK;
    int need = nb + (DU_BLOCK - tail < 0x100 ? 1 : 0);
    if (!du_guard(DU_FIRST_BLOCK + need - 1)) return DU_F_NOSPACE;
    uint8_t *last = NEW_ARR(DU_BLOCK);
    if (!last) return DU_F_WRITE;
    NAND_WAIT(); NAND_LOCK(0); NAND_ERASE_MAIN(); NAND_LOCK(1);
    int r = DU_F_OK;
    for (int i = 0; i < need && r == DU_F_OK; i++) {
        int lb = DU_FIRST_BLOCK + i;
        if (!du_guard(lb) || !du_guard(mapped(lb))) { r = DU_F_GUARD; break; }   /* never outside 4..131 */
        const uint8_t *src = ldr + (uint32_t)i * DU_BLOCK;
        if (i >= nb - 1) {                                         /* last data block / 0xFF end block */
            uint32_t len = i == nb - 1 ? tail : 0;
            for (uint32_t k = 0; k < DU_BLOCK; k++) last[k] = k < len ? src[k] : 0xFF;
            src = last;
        }
        NAND_WAIT(); NAND_LOCK(0);
        if (NAND_WRITE_BLOCK(lb, src)) r = DU_F_WRITE;            /* (writes, reads back, retries itself) */
        NAND_LOCK(1);
    }
    DEL_ARR(last);
    return r;
}

static uint8_t io[4096];
static int file_crc(const char *path, uint32_t size, uint32_t want)
{
    int fd = Y_OPEN(path, O_READ, 0x100);
    if (fd < 0) return 0;
    uint32_t got = 0, c = 0;
    for (;;) {
        int n = Y_READ(fd, io, sizeof io);
        if (n <= 0) break;
        c = du_crc32(c, io, (uint32_t)n); got += (uint32_t)n;
    }
    Y_CLOSE(fd);
    return got == size && c == want;
}

static void cat(char *o, const char *a, const char *b) { int k = 0; while (*a) o[k++] = *a++; while (*b) o[k++] = *b++; o[k] = 0; }

static int copy_file(const char *src, const char *final_rel)
{
    char dst[DU_MAX_PATH + 16];
    cat(dst, "/yaffs2/", final_rel);
    for (int k = 8; dst[k]; k++)                                   /* parent directories (errors: exists) */
        if (dst[k] == '/') { dst[k] = 0; Y_MKDIR(dst, 0x1FF); dst[k] = '/'; }
    int in = Y_OPEN(src, O_READ, 0x100);
    if (in < 0) return 0;
    int out = Y_OPEN(dst, O_CREATE, 0x180);
    if (out < 0) { Y_CLOSE(in); return 0; }
    int ok = 1;
    for (;;) {
        int n = Y_READ(in, io, sizeof io);
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;
        if (Y_WRITE(out, io, (unsigned)n) != n) { ok = 0; break; }
    }
    Y_CLOSE(out); Y_CLOSE(in);
    return ok;
}

static void cleanup(void)
{
    char name[40];
    for (int i = 0; i <= DU_MAX_FILES; i++) { du_stage_name(i, name); Y_UNLINK(name); }
    Y_UNLINK(DU_MANIFEST);
}

static du_session sess;
volatile int du_fw_result = -1;                                   /* last apply: 0 ok, else step (emulator tests) */

static int apply(void)
{
    char *text = du_text;                                          /* boot: no session yet, its buffers are free */
    du_file *f = sess.f;
    int fd = Y_OPEN(DU_MANIFEST, O_READ, 0x100);
    if (fd < 0) return 1;
    int n = Y_READ(fd, text, DU_TEXT_CAP - 1);
    Y_CLOSE(fd);
    if (n <= 0) return 2;
    text[n] = 0;
    int nf;
    if (du_manifest_parse(text, f, &nf)) return 3;
    char name[40];
    int ldr = -1;
    for (int i = 0; i < nf; i++) {                                 /* everything checked before anything changes */
        du_stage_name(i, name);
        if (f[i].size != DU_DEL_SIZE && !file_crc(name, f[i].size, f[i].crc)) return 10 + i;
        if (f[i].path[0] == 'O') ldr = i;                          /* (du_path_ok: "OP1_vdk.ldr" or "content/...") */
    }
    for (int i = 0; i < nf; i++) {
        if (i == ldr) continue;
        du_stage_name(i, name);
        CONSOLE(f[i].path);
        if (f[i].size == DU_DEL_SIZE) {                            /* delete (plugins/<name>.op1p, checked by parse) */
            char dst[DU_MAX_PATH + 16];
            cat(dst, "/yaffs2/", f[i].path);
            Y_UNLINK(dst);                                         /* missing: nothing to do */
            continue;
        }
        if (!copy_file(name, f[i].path)) return 100 + i;
    }
    if (ldr >= 0) {
        uint8_t *buf = NEW_ARR(f[ldr].size);
        if (!buf) return 200;
        du_stage_name(ldr, name);
        fd = Y_OPEN(name, O_READ, 0x100);
        uint32_t got = 0;
        if (fd >= 0) {
            for (;;) {
                int k = Y_READ(fd, buf + got, f[ldr].size - got > 65536 ? 65536 : f[ldr].size - got);
                if (k <= 0) break;
                got += (uint32_t)k;
            }
            Y_CLOSE(fd);
        }
        int r = got == f[ldr].size && du_crc32(0, buf, got) == f[ldr].crc ? (CONSOLE("Writing firmware..."), du_fw_flash(buf, got)) : 201;
        DEL_ARR(buf);
        if (r) return 300 + r;
    }
    return 0;
}

int du_boot_access(const char *path, int mode)
{
    static int done;
    if (!done) {
        done = 1;
        if (Y_ACCESS(DU_MANIFEST, 0) == 0) {
            CONSOLE("op1r: applying staged update...");
            int r = apply();
            du_fw_result = r;
            cleanup();
            if (r != 0 && r < 100) CONSOLE("op1r: staged update invalid, discarded.");   /* nothing changed: boot on */
            else {
                CONSOLE(r == 0 ? "op1r: update applied, restarting..." : "op1r: update FAILED (te-boot untouched), restarting...");
                UNMOUNT(); DELAY_MS(500);
                /* system reset by watchdog, as te-boot after an update (te-boot 0x000169EC). Not power-off: at this
                 * point nothing watches the soft power switch, and over USB the OP-1 stayed "off" (docs/35). */
                WDOG_CTL = 0xAD0; WDOG_CNT = 1; WDOG_CTL = 0xFF0;
                for (;;) ;
            }
        }
    }
    return Y_ACCESS(path, mode);
}

/* ---- transfer phase (running firmware, --dev only): docs/35 ----
 * PlayerManager::tick -> midi_poll: CALL midi_rx_parse (0x019E3CC8) -> du_rx_poll (timer context, no blocking):
 * copies our SysEx messages (F0 00 20 76 7D ...) from the USB-MIDI RX ring into a queue, then runs the stock parser
 * (which ignores them). The main loop (ci_service) -> du_service: du_handle with YAFFS2 writes, reply as USB-MIDI
 * SysEx packets. COMMIT shows a prompt over the current screen (screens/duprompt.c): T1 = manifest, reply, restart
 * (watchdog; the boot hook applies), T2 = staging discarded. */
#define RX_RING   ((volatile uint8_t *)0x01AB0510)                /* 1024 x 4-byte USB-MIDI packets */
#define RX_WR     (*(volatile uint32_t *)0x01AB1510)              /* free-running counters (& 0x3FF) */
#define RX_RD     (*(volatile uint32_t *)0x01AB1514)
#define RX_PARSE  ((void (*)(void))0x019E31A4)
#define TX_ENQ    ((void (*)(int, const void *, unsigned))0x019E0D18)   /* usbmidi_tx_enqueue(_, packets, count) */
#define SCREEN_TABLE   ((void **)0x01DF4E54)
#define CURRENT_SCREEN (*(volatile int *)0x018B65C8)
#define QN 16

static uint8_t q[QN][DU_MAX_MSG];
static uint16_t qlen[QN];
static volatile unsigned qw, qr;
static uint8_t as[DU_MAX_MSG];
static int an = -1;                                               /* bytes assembled, -1: outside a message */
static unsigned rx_pos;
volatile int du_fw_rx, du_fw_drop, du_fw_staged;                  /* counters for tests */

static void rx_byte(uint8_t b)
{
    if (b == 0xF0) an = 0;
    if (an < 0) return;
    if (an >= DU_MAX_MSG) { an = -1; return; }
    as[an++] = b;
    if (b != 0xF7) return;
    if (an > DU_SYSEX_HDR && as[1] == 0 && as[2] == 0x20 && as[3] == 0x76 && as[4] == 0x7D) {
        if (qw - qr < QN) {
            uint8_t *d = q[qw % QN];
            for (int i = 0; i < an; i++) d[i] = as[i];
            qlen[qw % QN] = (uint16_t)an; qw++; du_fw_rx++;
        } else du_fw_drop++;                                       /* host resends (DATA: offset reply) */
    }
    an = -1;
}

void du_rx_poll(void)
{
    unsigned wr = RX_WR;
    if (wr - rx_pos > 1024) rx_pos = RX_RD;                        /* first call / lapped */
    for (; rx_pos != wr; rx_pos++) {
        volatile uint8_t *p = RX_RING + (rx_pos & 0x3FF) * 4;
        int cin = p[0] & 15, k = cin == 5 ? 1 : cin == 6 ? 2 : (cin == 4 || cin == 7) ? 3 : 0;
        for (int i = 0; i < k; i++) rx_byte(p[1 + i]);
        extern void ms_midi_rx(const volatile uint8_t *pk);
        ms_midi_rx(p);                                             /* MIDI sequencer: input M<channel> (any channel) */
        extern void cc_rx(volatile uint8_t *pk);
        cc_rx(p);                                                  /* CC groups: keys / params channels (midi/ccmap.c) */
        extern void pc_filter(volatile uint8_t *pk);
        pc_filter(p);                                              /* pulse clock in: MIDI clock dropped (midi/pclk.c) */
    }
    extern void pc_poll(void);
    pc_poll();                                                     /* pulse clock in: ticks, start / stop */
    extern void cc_tx_poll(void);
    cc_tx_poll();                                                  /* CC feedback (midi/ccmap.c) */
    RX_PARSE();
}

static int fd_w = -1;
static int f_open(void *c, const char *p) { (void)c; Y_MKDIR(DU_STAGE_DIR, 0x1FF); fd_w = Y_OPEN(p, O_CREATE, 0x180); return fd_w < 0; }
static int f_write(void *c, const uint8_t *p, uint32_t n) { (void)c; return Y_WRITE(fd_w, p, n) != (int)n; }
static int f_close(void *c) { (void)c; int r = Y_CLOSE(fd_w); fd_w = -1; return r; }
static int f_remove(void *c, const char *p) { (void)c; return Y_UNLINK(p); }
/* ---- prompt: COMMIT -> "UPDATE READY", T1 restart / T2 cancel ---- */
void *du_prompt_obj;                                              /* screen object (dispatcher.c, boot) */
extern int dp_files, dp_kb, dp_restarting, dp_fw;
static volatile int prompt, confirmed, cancel_req, restart_req;
static so_owner du_own;                                           /* .obj = du_prompt_obj (set at boot) */

static void prompt_show(int on)                                   /* over the current screen, follows screen changes */
{
    int cur = CURRENT_SCREEN;
    if (du_own.in && (!on || du_own.id != cur || SCREEN_TABLE[du_own.id] != du_prompt_obj))
        so_restore(&du_own, SCREEN_TABLE);                         /* (covered: put back later by whoever covered it) */
    if (on && du_prompt_obj && !du_own.in && cur >= 0 && cur < 0x44) { du_own.obj = du_prompt_obj; so_install(&du_own, SCREEN_TABLE, cur); }
}

int du_prompt_active(void) { return prompt; }                    /* midi_cc_hook: no MIDI key events while shown */
int du_prompt_key(int code, int pressed)                          /* key hook (dispatcher.c): 1 = swallowed */
{
    if (!prompt || (code != 0x06 && code != 0x07)) return 0;
    if (pressed && !confirmed && !cancel_req) { if (code == 0x06) confirmed = 1; else cancel_req = 1; }
    return 1;
}

static int f_key(void *c)                                         /* COMMIT asks: first time shows the prompt */
{
    (void)c;
#ifdef OP1R_DEV
    return 1;                                                    /* developer builds (docs/86): no T1 confirmation */
#endif
    if (!prompt) {
        uint32_t b = 0;
        dp_fw = 0;
        for (int i = 0; i < sess.n; i++) { if (sess.f[i].size != DU_DEL_SIZE) b += sess.f[i].size; if (sess.f[i].path[0] == 'O') dp_fw = 1; }   /* OP1_vdk.ldr */
        dp_files = sess.n; dp_kb = (int)((b + 1023) / 1024); dp_restarting = 0;
        confirmed = cancel_req = 0; prompt = 1;
    }
    return confirmed;
}
static void f_reboot(void *c) { (void)c; du_fw_staged = 1; restart_req = 1; }   /* after the OK reply */
static int f_stat(void *c, const char *rel, uint32_t *size)      /* /yaffs2/<rel>: size (no data read) */
{
    (void)c;
    char p[DU_MAX_PATH + 16];
    cat(p, "/yaffs2/", rel);
    int fd = Y_OPEN(p, O_READ, 0x100);
    if (fd < 0) return 1;
    int n = Y_LSEEK(fd, 0, 2);
    Y_CLOSE(fd);
    if (n < 0) return 1;
    *size = (uint32_t)n;
    return 0;
}
static int f_read(void *c, const char *rel, uint32_t off, uint8_t *buf, uint32_t n)   /* /yaffs2/<rel> from off */
{
    (void)c;
    char p[DU_MAX_PATH + 16];
    cat(p, "/yaffs2/", rel);
    int fd = Y_OPEN(p, O_READ, 0x100);
    if (fd < 0) return -1;
    int k = Y_LSEEK(fd, (int)off, 0) == (int)off ? Y_READ(fd, buf, n) : -1;
    Y_CLOSE(fd);
    return k;
}
#define Y_OPENDIR  ((void *(*)(const char *))0x01A9B450)
#define Y_READDIR  ((uint8_t *(*)(void *))0x01A9B648)            /* dirent: d_name at +0xB */
#define Y_CLOSEDIR ((int (*)(void *))0x01A9B74C)
static int f_list(void *c, int index, char *name, int cap, uint32_t *size)   /* index-th plugins/ file */
{
    void *d = Y_OPENDIR("/yaffs2/plugins");
    if (!d) return 1;
    uint8_t *e;
    int r = 1;
    while ((e = Y_READDIR(d))) {
        const char *s = (const char *)e + 0xB;
        if (s[0] == '.' || index--) continue;
        int k = 0;
        while (s[k] && k < cap - 1) { name[k] = s[k]; k++; }
        name[k] = 0;
        char rel[DU_MAX_PATH];
        cat(rel, "plugins/", name);
        if (f_stat(c, rel, size)) *size = 0;
        r = 0;
        break;
    }
    Y_CLOSEDIR(d);
    return r;
}
#define BOOT_TEXT ((const char *)0x018F7358)                      /* make_firmware.py BOOT_TEXT_ADDR, 58 bytes */
static int f_ident(void *c, char *out, int cap)                   /* "op1r <n> [YYYY-MM-DD HH:MM]" (stock: "R. 00246") */
{
    (void)c;
    int i = 0, k = 0;
    while (i < 58 && (BOOT_TEXT[i] == '\n' || BOOT_TEXT[i] == ' ')) i++;
    while (i < 58 && BOOT_TEXT[i] && k < cap) out[k++] = BOOT_TEXT[i++];
    return k;
}
static const du_fs fw_fs = { f_open, f_write, f_close, f_remove, f_key, f_reboot, 0, f_stat, f_ident, f_list, f_read };

static void send_sysex(const uint8_t *m, int n)
{
    static uint32_t pk[DU_MAX_MSG / 3 + 2];
    int np = 0;
    for (int i = 0; i < n; i += 3) {
        int k = n - i < 3 ? n - i : 3, cin = n - i > 3 ? 4 : 4 + k;   /* 4: continues, 5/6/7: ends with 1/2/3 */
        uint32_t w = (uint32_t)cin;
        for (int j = 0; j < k; j++) w |= (uint32_t)m[i + j] << (8 * (j + 1));
        pk[np++] = w;
    }
    TX_ENQ(0, pk, (unsigned)np);
}

#ifdef OP1R_DEV
/* developer commands (local builds only, docs/86): read / write memory, inject keys and encoder ticks as the key-scan ISR
 * does (midi/ccmap.c). Reply: cmd | 0x40, seq, status, payload. Addresses: SDRAM, L1 data, scratchpad (no MMRs). */
enum { DV_PEEK = 12, DV_POKE = 13, DV_KEY = 14, DV_TURN = 15 };
#define DV_MAX 192
int du_dev_range_ok(uint32_t a, uint32_t n)
{
    const uint32_t e = a + n;
    if (n == 0 || n > DV_MAX || e < a) return 0;
    return e <= 0x04000000u || (a >= 0xFF800000u && e <= 0xFF908000u) || (a >= 0xFFB00000u && e <= 0xFFB01000u);
}
static uint32_t dv_irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void dv_irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
static int dev_cmd(const uint8_t *msg, int n, uint8_t *rep)       /* reply length, or -1: not a developer command */
{
    static const uint8_t hdr[DU_SYSEX_HDR] = { 0xF0, 0x00, 0x20, 0x76, 0x7D };
    if (n < DU_SYSEX_HDR + 4 || msg[n - 1] != 0xF7) return -1;
    for (int i = 0; i < DU_SYSEX_HDR; i++) if (msg[i] != hdr[i]) return -1;
    const int cmd = msg[5], seq = msg[6] | msg[7] << 7;
    if (cmd < DV_PEEK || cmd > DV_TURN) return -1;
    uint8_t pl[DU_MAX_MSG], b[1 + DV_MAX];
    const int pn = du_unpack7(msg + 8, n - 9, pl);
    int st = DU_OK, bn = 0;
    const uint32_t a = pn >= 4 ? (uint32_t)pl[0] | (uint32_t)pl[1] << 8 | (uint32_t)pl[2] << 16 | (uint32_t)pl[3] << 24 : 0;
    if (pn < 0) st = DU_E_FORMAT;
    else if (cmd == DV_PEEK) {                                   /* u32 addr, u16 len -> bytes */
        const uint32_t len = pn >= 6 ? (uint32_t)pl[4] | (uint32_t)pl[5] << 8 : 0;
        if (pn < 6 || !du_dev_range_ok(a, len)) st = DU_E_OFFSET;
        else for (uint32_t i = 0; i < len; i++) b[1 + bn++] = ((const volatile uint8_t *)a)[i];
    } else if (cmd == DV_POKE) {                                 /* u32 addr, bytes */
        if (pn < 5 || !du_dev_range_ok(a, (uint32_t)(pn - 4))) st = DU_E_OFFSET;
        else for (int i = 4; i < pn; i++) ((volatile uint8_t *)a)[i - 4] = pl[i];
    } else if (cmd == DV_KEY) {                                  /* u8 code, u8 down */
        if (pn < 2) st = DU_E_FORMAT;
        else {
            volatile uint32_t *rd = (volatile uint32_t *)0x01DD5244, *wr = (volatile uint32_t *)0x01DD5248;
            const uint32_t m = dv_irq_off(), w = *wr, nx = (w + 1) & 0xFF;
            if (nx != *rd && !*(volatile uint8_t *)0x01DD524D) ((volatile uint16_t *)0x01DD5044)[w] = (uint16_t)(pl[0] | (pl[1] ? 1 : 0) << 8), *wr = nx;
            else st = DU_E_FULL;
            dv_irq_on(m);
            ((void (*)(void))0x019C2F04)();                      /* KeyHandler::signal */
        }
    } else {                                                     /* DV_TURN: u8 knob 0..3, s8 ticks (2 per detent) */
        if (pn < 2 || pl[0] > 3) st = DU_E_FORMAT;
        else {
            const uint32_t m = dv_irq_off();
            ((volatile int32_t *)0x01DD5250)[pl[0]] += (int8_t)pl[1];
            dv_irq_on(m);
            ((void (*)(void))0x019C2F04)();
        }
    }
    b[0] = (uint8_t)st;
    return du_build(rep, cmd | DU_REPLY, seq, b, 1 + bn);
}
#endif

void du_service(void)
{
    static int inited;
    while (qr != qw) {
        if (!inited) { du_init(&sess, &fw_fs); inited = 1; }
        static uint8_t rep[DU_MAX_MSG];
#ifdef OP1R_DEV
        int n = dev_cmd(q[qr % QN], qlen[qr % QN], rep);
        if (n < 0) n = du_handle(&sess, q[qr % QN], qlen[qr % QN], rep);
#else
        int n = du_handle(&sess, q[qr % QN], qlen[qr % QN], rep);
#endif
        qr++;
        if (n > 0) send_sysex(rep, n);
    }
    if (cancel_req && inited) { du_abort(&sess); cancel_req = 0; prompt = 0; }   /* next COMMIT: STATE = cancelled */
    if (prompt || du_own.in) prompt_show(prompt);
    if (restart_req) {
        dp_restarting = 1;
        DELAY_MS(400);                                             /* reply out, "RESTARTING" drawn (GUI thread) */
        UNMOUNT();                                                 /* flushes all if nothing is open (else refused) */
        DELAY_MS(100);
        WDOG_CTL = 0xAD0; WDOG_CNT = 1; WDOG_CTL = 0xFF0;          /* system reset (as te-boot) -> boot hook applies */
        for (;;) ;
    }
}
