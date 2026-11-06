/* Crash screen (docs/65). The stock exception handler (0xFFA00380) prints "SEQSTAT : RETX" / "ADDR : STATUS" and calls
 * the frame-pointer printer 0x0191995C, which can only show the handler's own frames (the vector stub's CALL also
 * overwrites RETS). Hooked at its entry (make_firmware.py HOOKS hook_crash): the same message, then
 *   op1r <version> sp <stack pointer>
 *   the first values on the interrupted stack (above the vector stub's return address 0xFFA035A8, i.e. past the
 *   exception frames) that look like code addresses (stock firmware, op1r core, L1, plugin images): return
 *   addresses of the calls that led to the crash. Plugin addresses as file+offset, the others
 *   raw for tools/op1crash.py (with that release's synth.sym).
 * Exception context: no allocation, reads only plausible stack memory, text into the stock buffer. */
#include "op1_synth.h"

#define CRASH_TXT  ((char *)0x01BA138C)                         /* 512 bytes shown on the error screen */
#define CRASH_MAX  511
#define SNPRINTF   ((int (*)(char *, int, const char *, ...))0x01A4CCD8)
#define BOOT_TEXT  ((const char *)0x018F7358)                   /* "...op1r <n> [build time]" (make_firmware.py) */
struct plug_img { uint32_t base, size; char file[24]; };
extern struct plug_img plug_imgs[32];
extern uint32_t plug_img_n;
extern char _image_end[];                                 /* link.ld __image_end (C names get a leading _) */

static int plug_of(uint32_t v)
{
    for (uint32_t i = 0; i < plug_img_n && i < 32; i++) if (v - plug_imgs[i].base < plug_imgs[i].size) return (int)i;
    return -1;
}
static int code_addr(uint32_t v)
{
    if (v & 1) return 0;
    return (v >= 0x01900000 && v < 0x01B00000) || (v >= 0x01E00000 && v < (uint32_t)_image_end) ||
           (v >= 0xFFA00000 && v < 0xFFA14000) || plug_of(v) >= 0;
}
static char *add(char *p, const char *s) { while (*s && p < CRASH_TXT + CRASH_MAX) *p++ = *s++; *p = 0; return p; }

void crash_print(const char *msg)
{
    char *p = CRASH_TXT, t[48];
    while (*p && p < CRASH_TXT + CRASH_MAX) p++;
    p = add(add(p, msg), "\n");
    uint32_t sp;
    __asm__ volatile ("%0 = SP;" : "=d"(sp));
    const char *v = BOOT_TEXT;                                  /* the version from the boot text */
    for (int i = 0; i < 80 && v[i]; i++) if (v[i] == 'o' && v[i + 1] == 'p' && v[i + 2] == '1' && v[i + 3] == 'r') { v += i; break; }
    if (v[0] != 'o') v = "op1r ?";
    int n = 0; while (n < 20 && v[n] >= ' ') n++;
    char ver[24]; for (int i = 0; i < n; i++) ver[i] = v[i]; ver[n] = 0;
    SNPRINTF(t, sizeof t, "%s sp %#x\n", ver, sp);
    p = add(p, t);
    /* stack scan: only inside SDRAM or L1 scratch, 1 KB at most */
    uint32_t lo = sp, hi = sp + 1024;
    int ok = (lo >= 0x1000 && hi <= 0x02000000) || (lo >= 0xFFB00000 && lo < 0xFFB01000);
    if (ok && lo >= 0xFFB00000 && hi > 0xFFB01000) hi = 0xFFB01000;
    /* the exception frames (handler, kernel) lie below the vector stub's return address 0xFFA035A8: start after it */
    for (uint32_t a = lo; ok && a + 4 <= hi; a += 4)
        if (*(const volatile uint32_t *)a == 0xFFA035A8u) {
            lo = a + 4; hi = lo + 1024;
            if (hi > 0x02000000 && lo < 0x02000000) hi = 0x02000000;
            if (lo >= 0xFFB00000 && hi > 0xFFB01000) hi = 0xFFB01000;
            break;
        }
    int found = 0;
    for (uint32_t a = lo; ok && a + 4 <= hi && found < 10; a += 4) {
        uint32_t w = *(const volatile uint32_t *)a;
        if (!code_addr(w)) continue;
        int pi = plug_of(w);
        if (pi >= 0) {
            char f[16]; int k = 0;
            for (; k < 15 && plug_imgs[pi].file[k] && plug_imgs[pi].file[k] != '.'; k++) f[k] = plug_imgs[pi].file[k];
            f[k] = 0;
            SNPRINTF(t, sizeof t, "%s+%#x%s", f, w - plug_imgs[pi].base, found & 1 ? "\n" : " ");
        } else SNPRINTF(t, sizeof t, "%#x%s", w, found & 1 ? "\n" : " ");
        p = add(p, t);
        found++;
    }
    if (found & 1) p = add(p, "\n");
}
