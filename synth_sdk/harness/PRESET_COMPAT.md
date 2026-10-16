# Unsupported drum preset name fallback

The shared instrument-name comparison already maps unknown synth names to Iter.
That fallback is not a drum class. While the existing `dp_load_parse` wrapper is
parsing an actual drum-slot JSON object, unsupported nonempty names now resolve
to DBox instead. Literal stock names (including Iter) keep their own entries,
and the synth-load behavior remains unchanged. No individual plugin is named
or required by this policy.

The existing drum-load hook and stock parser remain in use. No range check,
parser error, resource guard or save operation is bypassed. The previous drum
slot scope is restored after nested calls and nonzero parser returns.

Run the firmware-free host regression from `synth_sdk`:

```sh
mkdir -p harness/build
cc -std=c11 -Wall -Wextra -Werror -g -O1 \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  harness/preset_compat_test.c -o harness/build/preset_compat_test
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1 harness/build/preset_compat_test
```

Coverage: all fifteen existing stock instrument names in both comparison orders
and load contexts, removed/foreign names, short and heap-backed byte buffers,
case sensitivity, empty/null inputs, no byte-buffer mutation, all eight synthetic
drum-slot addresses, out-of-range addresses, nested valid/invalid scopes, parser
error propagation and return to the normal synth fallback. The pure policy tests
do not reproduce the target's 32-bit string ABI or execute the firmware loader.

## Native qualification required before shipping

This is a bounded **name-resolution fix**, not a promise that any incompatible
preset can boot. A native maintainer test must establish:

- The boot restore calls traverse the existing hook with the expected drum-slot
  JSON addresses, and the compared instrument belongs to that scope. Nested
  synth parses that bypass the wrapper and cross-thread comparisons are not
  covered by the host test.
- A removed drum engine's DBox-compatible common fields load, then notes,
  release, slot switching and returning to the stock drum/synth still work.
- Incompatible parameter shapes still return native errors; no success is
  fabricated. Those errors can still reach the caller's boot assertion.
- Save/reload semantics and preservation of unsupported extension fields are
  established separately. The stock parser reconstructs JSON: this change does
  not create an original-file backup or promise lossless autosave. Back up user
  data before testing firmware changes.

Unknown FX/LFO/sequencer handling, selector collisions after plugin-order reuse,
malformed JSON recovery, storage recovery and booting unpatched stock firmware
are separate work. Do not generalize a successful name-policy test to them.
