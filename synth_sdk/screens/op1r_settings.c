/* Integer settings in op1.db, table op1rsettings (key TEXT PRIMARY KEY, val INTEGER): the one copy of the DB code the
 * pages share (settings, usb out, FX2 routing, scale). Firmware: GUI / KeyHandler threads only (sqlite). Host: an
 * in-memory table, so the host tests read back what a page saved. Keys are constants of our code (no quoting). */
#include "../op1_synth.h"
#include "op1r_settings.h"

#ifdef OP1_HOST
static struct { const char *k; int v; } tab[32];
static int ntab;
static int find(const char *k)
{
    for (int i = 0; i < ntab; i++) { const char *a = tab[i].k, *b = k; while (*a && *a == *b) a++, b++; if (!*a && !*b) return i; }
    return -1;
}
int op1r_setting_get(const char *key, int dflt) { int i = find(key); return i < 0 ? dflt : tab[i].v; }
void op1r_setting_put(const char *key, int val)
{
    int i = find(key);
    if (i < 0) { if (ntab == 32) return; i = ntab++; tab[i].k = key; }
    tab[i].v = val;
}
void op1r_setting_host_clear(void) { ntab = 0; }
#else
#define DB_HANDLE ((void *(*)(void))0x0196DD44)
#define DB_EXEC   ((void (*)(void *, const char *))0x0196F950)
#define DB_GET    ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } dstr;
static const char CREATE[] = "CREATE TABLE IF NOT EXISTS op1rsettings (key TEXT PRIMARY KEY, val INTEGER NOT NULL)";

static char *put_s(char *e, const char *s) { while (*s) *e++ = *s++; return e; }
static char *put_i(char *e, int v)
{
    char d[12]; int n = 0; unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    if (v < 0) *e++ = '-';
    do { d[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) *e++ = d[--n];
    return e;
}

int op1r_setting_get(const char *key, int dflt)
{
    static char q[96];
    *put_s(put_s(put_s(q, "SELECT val FROM op1rsettings WHERE key='"), key), "'") = 0;
    void *db = DB_HANDLE();
    DB_EXEC(db, CREATE);
    dstr s = { 0 };
    DB_GET(&s, db, q);
    const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;         /* (heap copy for long strings, not freed: short values) */
    int n = (int)s.size, i = 0, neg = 0, v = 0;
    if (i < n && p[i] == '-') neg = 1, i++;
    if (i >= n || p[i] < '0' || p[i] > '9') return dflt;
    for (; i < n && p[i] >= '0' && p[i] <= '9'; i++) v = v * 10 + p[i] - '0';
    return neg ? -v : v;
}

void op1r_setting_put(const char *key, int val)
{
    static char q[112];
    char *e = put_s(q, "INSERT OR REPLACE INTO op1rsettings VALUES ('");
    e = put_i(put_s(put_s(e, key), "',"), val);
    *put_s(e, ")") = 0;
    void *db = DB_HANDLE();
    DB_EXEC(db, CREATE);
    DB_EXEC(db, q);
}
#endif
