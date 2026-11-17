/* Patch saves that fit (after hundekraft 3.3 kennel/kn_patch_sql.c, Hundekraft, GPL-3.0-or-later with the OP-1
 * firmware permission). R246's patch jobs (BgFileWorker: kit / preset loads, type jobs, slot save) format
 *     snprintf(0x018C7A40, 5000, "UPDATE ..._slots SET patch='%s' WHERE id=%i", patch, slot)
 * and run it with db2_exec, which asserts on any SQLite error (db2.cpp). A patch over ~4,950 bytes is cut before its
 * closing quote ("unrecognized token"), and a ' in a patch string is not escaped. Each of the seven snprintf CALLs
 * (make_firmware.py HOOKS hook_psql1..7) comes here with the same arguments: the statement is built with every '
 * doubled; if it fits the stock buffer it goes there as before; else it is built in our buffer and run here (the same
 * db2_exec on op1.db), and the stock buffer gets a statement that does nothing. Past our buffer too: not saved (the
 * sound stays as it is in memory), counted. One thread (BgFileWorker, or the flush after it): one static buffer. */
#include <stdint.h>
#ifdef OP1_HOST
int psql_host_snprintf(char *, unsigned, const char *, const char *, int);
void psql_host_exec(const char *);
#define STOCK_SNPRINTF psql_host_snprintf
#define EXEC(sql)      psql_host_exec(sql)
#else
#define STOCK_SNPRINTF ((int (*)(char *, unsigned, const char *, ...))0x01A4CCD8)
#define EXEC(sql)      ((void (*)(void *, const char *))0x0196F950)(((void *(*)(void))0x0196DD44)(), (sql))   /* db2_exec(op1.db) */
#endif
#define PSQL_CAP 8192u
static char big[PSQL_CAP];
volatile uint32_t psql_big, psql_refused;                               /* counts (tests, monitor) */
static const char nothing[] = "SELECT 1 WHERE 0;";

/* the statement fmt makes, ' doubled in %s, written to out unless 0; its length, -1 for a format this does not know */
static int build(char *out, const char *fmt, const char *patch, int id)
{
    int n = 0;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { if (out) out[n] = *f; n++; continue; }
        f++;
        if (*f == '%') { if (out) out[n] = '%'; n++; }
        else if (*f == 's') {
            for (const char *p = patch ? patch : ""; *p; p++) {
                if (out) out[n] = *p;
                n++;
                if (*p == '\'') { if (out) out[n] = '\''; n++; }
            }
        } else if (*f == 'i' || *f == 'd') {
            char d[12]; int k = 0; uint32_t v = id < 0 ? 0u - (uint32_t)id : (uint32_t)id;
            do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
            if (id < 0) { if (out) out[n] = '-'; n++; }
            while (k--) { if (out) out[n] = d[k]; n++; }
        } else return -1;
    }
    if (out) out[n] = 0;
    return n;
}

int patch_sql(char *buf, unsigned cap, const char *fmt, const char *patch, int id)
{
    int n = build(0, fmt, patch, id);
    if (n < 0) return STOCK_SNPRINTF(buf, cap, fmt, patch, id);           /* not the known format: as before */
    if ((unsigned)n < cap) { build(buf, fmt, patch, id); return n; }
    if (cap <= sizeof nothing) return STOCK_SNPRINTF(buf, cap, fmt, patch, id);
    if ((unsigned)n < sizeof big) { build(big, fmt, patch, id); EXEC(big); psql_big++; }
    else psql_refused++;
    for (unsigned i = 0; i < sizeof nothing; i++) buf[i] = nothing[i];
    return n;
}
