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
int psql_host_snprintf3(char *, unsigned, const char *, uintptr_t, uintptr_t, uintptr_t);
void psql_host_exec(const char *);
#define STOCK_SNPRINTF  psql_host_snprintf
#define STOCK_SNPRINTF3 psql_host_snprintf3
#define EXEC(sql)       psql_host_exec(sql)
#else
#define STOCK_SNPRINTF  ((int (*)(char *, unsigned, const char *, ...))0x01A4CCD8)
#define STOCK_SNPRINTF3 STOCK_SNPRINTF
#define EXEC(sql)       ((void (*)(void *, const char *))0x0196F950)(((void *(*)(void))0x0196DD44)(), (sql))   /* db2_exec(op1.db) */
#endif
#define PSQL_CAP 8192u
static char big[PSQL_CAP];                                              /* BgFileWorker's patch jobs */
static char big_insert[2 * PSQL_CAP];                                   /* the inserts (boot / main thread) */
volatile uint32_t psql_big, psql_refused;                               /* counts (tests, monitor) */
static const char nothing[] = "SELECT 1 WHERE 0;";

/* the statement fmt makes from the arguments in order (%s a string with every ' doubled, %i / %d an int), written to out
 * unless 0; its length, -1 for a format this does not know (anything but %s %i %d %%, or more than 3 arguments) */
static int build(char *out, const char *fmt, const uintptr_t *arg)
{
    int n = 0, k = 0;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { if (out) out[n] = *f; n++; continue; }
        f++;
        if (*f == '%') { if (out) out[n] = '%'; n++; continue; }
        if ((*f != 's' && *f != 'i' && *f != 'd') || k == 3) return -1;
        uintptr_t a = arg[k++];
        if (*f == 's') {
            for (const char *p = a ? (const char *)a : ""; *p; p++) {
                if (out) out[n] = *p;
                n++;
                if (*p == '\'') { if (out) out[n] = '\''; n++; }
            }
        } else {
            int id = (int)a;
            char d[12]; int m = 0; uint32_t v = id < 0 ? 0u - (uint32_t)id : (uint32_t)id;
            do { d[m++] = (char)('0' + v % 10u); v /= 10u; } while (v);
            if (id < 0) { if (out) out[n] = '-'; n++; }
            while (m--) { if (out) out[n] = d[m]; n++; }
        }
    }
    if (out) out[n] = 0;
    return n;
}

/* into buf when it fits; else built in own[] and run here, buf a no-op; past own[] nothing runs. -1: unknown format */
static int place(char *buf, unsigned cap, const char *fmt, const uintptr_t *arg, char *own, unsigned own_cap)
{
    int n = build(0, fmt, arg);
    if (n < 0 || cap <= sizeof nothing) return -1;
    if ((unsigned)n < cap) { build(buf, fmt, arg); return n; }
    if ((unsigned)n < own_cap) { build(own, fmt, arg); EXEC(own); psql_big++; }
    else psql_refused++;
    for (unsigned i = 0; i < sizeof nothing; i++) buf[i] = nothing[i];
    return n;
}

int patch_sql(char *buf, unsigned cap, const char *fmt, const char *patch, int id)
{
    uintptr_t arg[3] = { (uintptr_t)patch, (uintptr_t)id, 0 };
    int n = place(buf, cap, fmt, arg, big, sizeof big);
    return n < 0 ? STOCK_SNPRINTF(buf, cap, fmt, patch, id) : n;        /* not the known format: as before */
}

/* R246's inserts with user text (after hundekraft 3.3.2: a ' in a kit name or folder copied in disk mode made
 * 'near "...": syntax error' and db2_exec's assert): snprintf(buf, 8192, ...) at 0x019956F4 (user preset,
 * "INSERT INTO %s (patch,folder) VALUES('%s','%s')": table, patch, folder), 0x01995AA6 / 0x01995CEE (slot rows,
 * "INSERT INTO synth_slots / drum_slots VALUES(%i,'%s')": id, patch). Boot / main thread (disk sync): own buffer. */
static int insert3(char *buf, unsigned cap, const char *fmt, uintptr_t a, uintptr_t b, uintptr_t c)
{
    uintptr_t arg[3] = { a, b, c };
    int n = place(buf, cap, fmt, arg, big_insert, sizeof big_insert);
    return n < 0 ? STOCK_SNPRINTF3(buf, cap, fmt, a, b, c) : n;
}
int insert_sql(char *buf, unsigned cap, const char *fmt, const char *table, const char *patch, const char *folder)
{ return insert3(buf, cap, fmt, (uintptr_t)table, (uintptr_t)patch, (uintptr_t)folder); }
int slot_insert_sql(char *buf, unsigned cap, const char *fmt, int id, const char *patch)
{ return insert3(buf, cap, fmt, (uintptr_t)id, (uintptr_t)patch, 0); }
