/* Settings page (clock / card + T3, docs/25), in the style of the playmode page like the USB out page: blue
 * "COUNT-IN" with 4 / 8 / 16 beats (tape/countin.c). Blue turns through the values; saved in op1rsettings
 * (key 'countin'). Green: DEBOUNCE OFF / 1..30 ms, saved as 'debounce_ms'. */
#include "../op1_synth.h"
#include "../input/debounce.h"
#include "stroke_font.h"

extern uint8_t ci_beats;
static const uint8_t ci_vals[3] = { 4, 8, 16 };
static uint8_t ci_idx;                                      /* index into ci_vals */

static unsigned st_parse_debounce(const char *p, uint32_t size)
{
    if (!size || size > 2) return KEY_DEBOUNCE_DEFAULT_MS;
    unsigned ms = 0;
    for (uint32_t i = 0; i < size; ++i) {
        if (p[i] < '0' || p[i] > '9') return KEY_DEBOUNCE_DEFAULT_MS;
        ms = ms * 10 + (unsigned)(p[i] - '0');
    }
    return ms <= KEY_DEBOUNCE_MAX_MS ? ms : KEY_DEBOUNCE_DEFAULT_MS;
}

#ifdef OP1_HOST
int settings_host_saved = -1;
const char *settings_host_debounce_saved;
static void st_load(void)
{
    if (settings_host_saved >= 0 && settings_host_saved <= 2) ci_idx = (uint8_t)settings_host_saved;
    uint32_t n = 0;
    if (settings_host_debounce_saved) while (settings_host_debounce_saved[n]) ++n;
    key_input_set_debounce_ms(st_parse_debounce(settings_host_debounce_saved, n));
}
static void st_save(int knob)
{
    if (!knob) settings_host_saved = ci_idx;
    else {
        static char saved[3];
        unsigned ms = key_input_debounce_ms();
        saved[0] = (char)('0' + ms / 10); saved[1] = (char)('0' + ms % 10); saved[2] = 0;
        settings_host_debounce_saved = saved;
    }
}
#else
#define DB_HANDLE ((void *(*)(void))0x0196DD44)
#define DB_EXEC   ((void (*)(void *, const char *))0x0196F950)
#define DB_GET    ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } st_dstr;
static const char ST_CREATE[] = "CREATE TABLE IF NOT EXISTS op1rsettings (key TEXT PRIMARY KEY, val INTEGER NOT NULL)";
static void st_load(void)                                   /* boot, screen init */
{
    void *db = DB_HANDLE();
    DB_EXEC(db, ST_CREATE);
    st_dstr s = { 0 };
    DB_GET(&s, db, "SELECT val FROM op1rsettings WHERE key='countin'");
    const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
    if (s.size && p[0] >= '0' && p[0] <= '2') ci_idx = (uint8_t)(p[0] - '0');
    st_dstr d = { 0 };
    /* Bound the result to two digits, even if a corrupt setting contains a long string. */
    DB_GET(&d, db, "SELECT val FROM op1rsettings WHERE key='debounce_ms' AND typeof(val)='integer' AND val BETWEEN 0 AND 30");
    p = d.cap > 15 ? d.u.ptr : d.u.buf;
    key_input_set_debounce_ms(st_parse_debounce(p, d.size));
}
static void st_save(int knob)                               /* KeyHandler thread, on change */
{
    static char q[] = "INSERT OR REPLACE INTO op1rsettings VALUES ('countin',0)";
    static char dq[] = "INSERT OR REPLACE INTO op1rsettings VALUES ('debounce_ms',00)";
    q[sizeof q - 3] = (char)('0' + ci_idx);
    unsigned ms = key_input_debounce_ms();
    dq[sizeof dq - 4] = (char)('0' + ms / 10);
    dq[sizeof dq - 3] = (char)('0' + ms % 10);
    void *db = DB_HANDLE();
    DB_EXEC(db, ST_CREATE);
    DB_EXEC(db, knob ? dq : q);
}
#endif

static int32_t acc[2];
void settings_turn(int knob, int delta)                     /* one value per detent, clamped */
{
    if (knob < 0 || knob > 1) return;
    if ((acc[knob] > 0 && delta < 0) || (acc[knob] < 0 && delta > 0)) acc[knob] = 0;
    acc[knob] += delta;
    int st = acc[knob] / 1024; if (!st) return;
    acc[knob] = 0;
    int v = (knob ? (int)key_input_debounce_ms() : ci_idx) + (st > 0 ? 1 : -1);
    if (v < 0 || v > (knob ? (int)KEY_DEBOUNCE_MAX_MS : 2)) return;
    if (knob) key_input_set_debounce_ms((unsigned)v);
    else { ci_idx = (uint8_t)v; ci_beats = ci_vals[ci_idx]; }
    st_save(knob);
}

static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF };
static const uint8_t c_green[4] = { 0x00, 0xED, 0x95, 0xFF };
#define ST_S   OP1_Q10(5) / 2                               /* as the USB out page: 15 px caps */
#define ST_W   0x600
#define ST_LX  OP1_Q10(24)
#define ST_VX  OP1_Q10(164)
#define ST_Y0  OP1_Q10(40)

static void st_init(void *scr) { (void)scr; st_load(); ci_beats = ci_vals[ci_idx]; }
static void st_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    static const char *const txt[3] = { "4", "8", "16" };
    sf_text(r, ST_LX, ST_Y0, ST_S, "COUNT-IN", ST_W, c_blue);
    sf_text(r, ST_VX, ST_Y0, ST_S, txt[ci_idx], ST_W, c_blue);
    unsigned ms = key_input_debounce_ms();
    char value[6]; unsigned n = 0;
    if (ms >= 10) value[n++] = (char)('0' + ms / 10);
    value[n++] = (char)('0' + ms % 10);
    value[n++] = ' '; value[n++] = 'M'; value[n++] = 'S'; value[n] = 0;
    sf_text(r, ST_LX, ST_Y0 + OP1_Q10(32), ST_S, "DEBOUNCE", ST_W, c_green);
    sf_text(r, ST_VX, ST_Y0 + OP1_Q10(32), ST_S, ms ? value : "OFF", ST_W, c_green);
}
const op1_screen_t SCREEN_settings = { st_init, st_update };
