/* Host test of patch_sql.c: a patch that fits goes to the stock buffer with ' doubled; a longer one runs from our
 * buffer and the stock buffer gets a no-op; past our buffer nothing runs; an unknown format falls back.
 *   cc -DOP1_HOST psql_test.c ../patch_sql.c */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int patch_sql(char *buf, unsigned cap, const char *fmt, const char *patch, int id);
extern volatile uint32_t psql_big, psql_refused;
static char ran[9000]; static int nexec, nsnp;
int psql_host_snprintf(char *b, unsigned c, const char *f, const char *p, int i) { nsnp++; return snprintf(b, c, f, p, i); }
void psql_host_exec(const char *s) { nexec++; strncpy(ran, s, sizeof ran - 1); }
static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL %s\n", m); } } while (0)
static const char F[] = "UPDATE synth_slots SET patch='%s' WHERE id=%i";
int main(void)
{
    static char buf[5000], pat[7000];
    patch_sql(buf, sizeof buf, F, "{\"name\":\"it's\"}", 3);
    CHECK(!strcmp(buf, "UPDATE synth_slots SET patch='{\"name\":\"it''s\"}' WHERE id=3"), "fits, quote doubled");
    CHECK(!nexec, "fits: not run here");
    memset(pat, 'a', 5200); pat[5200] = 0;
    int n = patch_sql(buf, sizeof buf, F, pat, 7);
    CHECK(nexec == 1 && psql_big == 1 && strlen(ran) == (size_t)n && !strcmp(ran + n - 12, "' WHERE id=7"), "long: run from ours");
    CHECK(!strcmp(buf, "SELECT 1 WHERE 0;"), "long: stock buffer a no-op");
    static char huge[9000]; memset(huge, 'b', 8500); huge[8500] = 0;
    patch_sql(buf, sizeof buf, F, huge, 1);
    CHECK(nexec == 1 && psql_refused == 1 && !strcmp(buf, "SELECT 1 WHERE 0;"), "too long: refused, nothing run");
    patch_sql(buf, sizeof buf, "x=%f", "", 0);
    CHECK(nsnp == 1, "unknown format: stock snprintf");
    patch_sql(buf, sizeof buf, F, "{}", -4);
    CHECK(!strcmp(buf, "UPDATE synth_slots SET patch='{}' WHERE id=-4"), "negative id");
    printf(fails ? "psql: %d failed\n" : "psql: ok\n", fails);
    return fails != 0;
}
