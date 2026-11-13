/* Host test for safe mode (safeboot.c): the key decision from the expander pins, and the op1r stores that honour
 * op1r_safe_boot (screens/op1r_settings.c, tape/markers.c). The plugin loader and the MIDI sequencer file are
 * firmware only (plugload.c, seqdev/ms_fw.c); the note on screen is in overlay_test.c.
 *   cc -DOP1_HOST -I.. safeboot_test.c ../safeboot.c */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../screens/op1r_settings.c"
volatile uint8_t pu_mode;
#include "../tape/markers.c"
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c) { (void)r; (void)p0; (void)p1; (void)w; (void)c; }
void *host_raster(void) { return 0; }

int sb_decide(const uint8_t *k);
/* GPIOA / GPIOB of 0x23..0x26 with the given key codes down (active low), the other pins as idle */
static int keys(int n, const int *code, uint8_t rest)
{
    uint8_t k[8];
    memset(k, 0xFF, sizeof k);
    k[7] = rest; k[6] = (uint8_t)(0xC0 & rest) | 0x3F;           /* 0x26: not key pins (any level) */
    for (int i = 0; i < n; i++) k[code[i] / 8] &= (uint8_t)~(1u << (code[i] % 8));
    return sb_decide(k);
}

int main(void)
{
    /* the decision: exactly help (0x00), or help + metronome (0x01); anything else is a normal boot */
    static const int help[] = { 0x00 }, both[] = { 0x00, 0x01 }, metro[] = { 0x01 }, help_shift[] = { 0x00, 0x26 },
        help_key[] = { 0x00, 0x30 }, help_com[] = { 0x00, 0x2C }, all3[] = { 0x00, 0x01, 0x02 };
    assert(keys(0, 0, 0xFF) == 0);
    assert(keys(1, help, 0xFF) == SB_ON);
    assert(keys(1, help, 0x00) == SB_ON);                         /* 0x26 B, A 6..7 ignored */
    assert(keys(2, both, 0xFF) == (SB_ON | SB_RESET));
    assert(keys(1, metro, 0xFF) == 0);
    assert(keys(2, help_shift, 0xFF) == 0 && keys(2, help_key, 0xFF) == 0 && keys(2, help_com, 0xFF) == 0);
    assert(keys(3, all3, 0xFF) == 0);
    static const uint8_t zero[8], ones[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    assert(sb_decide(zero) == 0 && sb_decide(ones) == 0);         /* mirror never filled / nothing held */
    for (int c = 0; c < 0x36; c++) {                              /* every single key: only help */
        int one[] = { c };
        assert(keys(1, one, 0xFF) == (c == 0 ? SB_ON : 0));
    }
    puts("decision ok");

    /* settings: stored values ignored (defaults), nothing written; the stored rows are back on a normal boot */
    op1r_setting_put("kbvel", 90);
    assert(op1r_setting_get("kbvel", 127) == 90);
    op1r_safe_boot = SB_ON;
    assert(op1r_setting_get("kbvel", 127) == 127 && op1r_setting_get("usbtracks", 0) == 0);
    op1r_setting_put("kbvel", 30); op1r_setting_put("scale", 5);
    op1r_safe_boot = 0;
    assert(op1r_setting_get("kbvel", 127) == 90 && op1r_setting_get("scale", -1) == -1);
    puts("settings ok");

    /* markers: not loaded, edits not saved */
    strcpy(tm_host_saved, "88200,441000");
    op1r_safe_boot = SB_ON;
    tm_host_cursor = 20 * 44100; tm_add();
    assert(tm_n == 1 && tm_pos[0] == 20 * 44100);                 /* the session's own marker */
    assert(!strcmp(tm_host_saved, "88200,441000"));
    tm_remove_at(20 * 44100, 10);
    assert(tm_n == 0 && !strcmp(tm_host_saved, "88200,441000"));
    puts("markers ok");
    printf("safeboot_test: ok\n");
    return 0;
}
