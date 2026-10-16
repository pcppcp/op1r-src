/* Shared loader policy regression; no firmware or private profiles required.
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
 *    harness/preset_compat_test.c -o harness/build/preset_compat_test */
#include "../preset_compat.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
#define CHECK(x) do { assert(x); checks++; } while (0)
static const char *const names[] = {
    "fm", "digital", "drwave", "string", "cluster", "pulse", "sampler",
    "phase", "drum", "dbox", "ls", "dna", "dsynth", "voltage", "iter"
};
static int select_type(const char *s, uint32_t n, int drum, int iter_first)
{
    /* Exercise table order, not just one successful comparison in isolation. */
    if (iter_first && op1_preset_instr_eq("iter", s, n, drum)) return 14;
    for (unsigned i = 0; i < 15; i++)
        if ((!iter_first || i != 14) && op1_preset_instr_eq(names[i], s, n, drum)) return (int)i;
    return -1;
}

static int loading = -1;
static unsigned first = 0x1004;       /* synthetic address, not firmware memory */
static int inner_slot, inner_result;
static int inner(void *json, int mode)
{
    CHECK(loading == inner_slot);
    CHECK(mode == 23);
    CHECK(json == (void *)(uintptr_t)0xCAFE);
    return inner_result;
}
static int outer(void *json, int mode)
{
    CHECK(loading == 2 && mode == 17 && json == (void *)(uintptr_t)0xBEEF);
    inner_slot = 5; inner_result = 0;
    CHECK(op1_preset_parse_scoped(&loading, inner_slot, (void *)(uintptr_t)0xCAFE, 23, inner) == 0);
    CHECK(loading == 2);
    inner_slot = -1; inner_result = 27;
    CHECK(op1_preset_parse_scoped(&loading, inner_slot, (void *)(uintptr_t)0xCAFE, 23, inner) == 27);
    CHECK(loading == 2);
    CHECK(op1_preset_instr_eq("dbox", "removed-engine", 14, loading >= 0));
    return 31;                       /* native error is preserved, not swallowed */
}
int main(void)
{
    for (unsigned i = 0; i < 15; i++) for (int drum = 0; drum < 2; drum++) for (int order = 0; order < 2; order++) {
        uint32_t n = (uint32_t)strlen(names[i]);
        CHECK(select_type(names[i], n, drum, order) == (int)i);
        CHECK(op1_preset_instr_eq(names[i], names[i], n, drum));
    }
    const char *const foreign[] = { "removed", "external-drum", "future-engine", "iter-plus", "dbox-plus", "ITER", "FM" };
    for (unsigned i = 0; i < sizeof foreign / sizeof *foreign; i++) for (int order = 0; order < 2; order++) {
        uint32_t n = (uint32_t)strlen(foreign[i]);
        CHECK(select_type(foreign[i], n, 1, order) == 9);    /* DBox, never Iter */
        CHECK(select_type(foreign[i], n, 0, order) == 14);   /* existing synth fallback */
    }
    char inline_bytes[16] = "short-unknown", before[16]; memcpy(before, inline_bytes, 16);
    CHECK(op1_preset_instr_eq("dbox", inline_bytes, 13, 1));
    CHECK(!memcmp(before, inline_bytes, 16));
    const char *long_name = "removed-engine-with-a-long-name";
    size_t n = strlen(long_name); char *heap = malloc(n + 1); CHECK(heap != 0); memcpy(heap, long_name, n + 1);
    CHECK(op1_preset_instr_eq("dbox", heap, (uint32_t)n, 1));
    CHECK(!memcmp(heap, long_name, n + 1)); free(heap);
    CHECK(!op1_preset_instr_eq("dbox", "", 0, 1));
    CHECK(!op1_preset_instr_eq("iter", "", 0, 0));
    CHECK(op1_preset_instr_eq("", "", 0, 0));               /* existing exact-empty behavior */
    CHECK(!op1_preset_instr_eq("dbox", 0, 8, 1));
    CHECK(!op1_preset_instr_eq(0, "removed", 7, 1));
    CHECK(!op1_preset_instr_eq("iterator", "removed", 7, 0));
    CHECK(!op1_preset_instr_eq("dbox-extra", "removed", 7, 1));
    CHECK(op1_preset_eq("iter", 4, "iter"));
    CHECK(!op1_preset_eq("iter", 4, "iterator"));
    CHECK(!op1_preset_eq("iterator", 8, "iter"));
    for (unsigned i = 0; i < 8; i++) CHECK(op1_preset_drum_slot(first + i * 0x38, first, 0x38, 8) == (int)i);
    CHECK(op1_preset_drum_slot(first - 1, first, 0x38, 8) == -1);
    CHECK(op1_preset_drum_slot(first + 1, first, 0x38, 8) == -1);
    CHECK(op1_preset_drum_slot(first + 8 * 0x38, first, 0x38, 8) == -1);
    CHECK(op1_preset_drum_slot(UINT32_MAX, first, 0x38, 8) == -1);
    CHECK(op1_preset_drum_slot(first, first, 0, 8) == -1);
    CHECK(op1_preset_drum_slot(first, first, 0x38, 0) == -1);
    CHECK(op1_preset_parse_scoped(&loading, 2, (void *)(uintptr_t)0xBEEF, 17, outer) == 31);
    CHECK(loading == -1);
    CHECK(op1_preset_instr_eq("iter", "removed-engine", 14, loading >= 0));
    CHECK(!op1_preset_instr_eq("dbox", "removed-engine", 14, loading >= 0));
    printf("preset compatibility: %d checks passed (policy/scope only; no native boot claim)\n", checks);
    return 0;
}
