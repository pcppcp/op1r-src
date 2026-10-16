/* Shared instrument-name fallback policy. No firmware calls or file writes. */
#ifndef OP1_PRESET_COMPAT_H
#define OP1_PRESET_COMPAT_H
#include <stdint.h>

static inline int op1_preset_eq(const char *bytes, uint32_t size, const char *lit)
{
    if (!bytes || !lit) return 0;
    for (uint32_t i = 0; i < size; i++)
        if (!lit[i] || bytes[i] != lit[i]) return 0;
    return !lit[size];
}

/* Exact stock names always keep their own table entry, irrespective of order.
 * Custom synths already use Iter; an unsupported name in a qualified drum load
 * needs a drum class instead. This does not validate the rest of the preset. */
static inline int op1_preset_instr_eq(const char *table_name, const char *bytes,
                                      uint32_t size, int drum_loading)
{
    static const char *const stock[] = {
        "fm", "digital", "drwave", "string", "cluster", "pulse", "sampler",
        "phase", "drum", "dbox", "ls", "dna", "dsynth", "voltage", "iter", 0
    };
    if (op1_preset_eq(bytes, size, table_name)) return 1;
    if (!bytes || !table_name || !size) return 0;
    if (!op1_preset_eq(table_name, 4u,
                       drum_loading ? "dbox" : "iter") || table_name[4]) return 0;
    for (const char *const *n = stock; *n; n++)
        if (op1_preset_eq(bytes, size, *n)) return 0;
    return 1;
}

/* Only the actual drum-slot JSON objects qualify. An arbitrary parser call is
 * not a drum-slot load. Address arithmetic never dereferences firmware memory. */
static inline int op1_preset_drum_slot(uint32_t json_addr, uint32_t first_json,
                                      uint32_t stride, unsigned slots)
{
    if (!stride || json_addr < first_json) return -1;
    uint32_t off = json_addr - first_json;
    return off % stride == 0 && off / stride < slots ? (int)(off / stride) : -1;
}

/* The native parser stays authoritative, including nonzero errors. Restore an
 * outer scope after a nested call rather than always clearing it to -1. */
static inline int op1_preset_parse_scoped(int *loading_slot, int slot, void *json,
                                         int mode, int (*parse)(void *, int))
{
    int previous = *loading_slot;
    *loading_slot = slot;
    int result = parse(json, mode);
    *loading_slot = previous;
    return result;
}
#endif
