# Key debounce

`debounce.c` implements a per-key filter for the physical keyboard, front-panel buttons and encoder presses.
The first press is delivered immediately. A release is delivered after the contact has remained released for
5 ms; a press inside that window cancels the release without sending another press. Duplicate states are ignored.
After a settled release, the next press is immediate again. A deliberate release/repress gap shorter than 5 ms
is indistinguishable from bounce and is merged into the held note. Encoder turns are handled separately.

`KEY_DEBOUNCE_CYCLES` in `debounce.h` sets the interval, using the same 400 MHz CPU-clock assumption as the
existing firmware code. Unsigned elapsed-time arithmetic handles a counter wrap during settling. Held keys do
not depend on the age of their initial press. Timing is measured when the key thread processes edges, not in
the physical scanner; long stalls of that thread can affect debounce timing.

## Firmware integration

`key_input.c` owns the state entirely in the stock key thread. It forwards accepted events through the existing
`key_event_hook`, so the stock handlers and custom shortcuts see the same filtered stream.

- `hook_keyev` calls `key_input_event` instead of dispatching directly.
- `hook_keywait` services deferred releases before the key thread's semaphore wait. While a release is pending,
  the wait uses one VDK tick with `kNoTimeoutError` (`0x80000000`), then the stock loop runs again. This ensures
  a final release is delivered without another key event. Once settled, the original indefinite wait is retained.
  The actual release latency is the settling interval plus scheduling/tick latency.
- An already nonempty key queue is drained before the wait hook expires releases. Incoming events also expire
  mature releases before dispatch, preserving modifier release ordering. If several releases mature together,
  they are delivered oldest first.
- MIDI CC and MIDI-sequencer producers set `KEY_SYNTHETIC` in the low queue byte. The adapter removes the tag
  and dispatches directly without updating physical debounce state. Mixer automation's private `MX_KEY` also
  bypasses the filter. Existing direct calls to the stock dispatcher are unaffected.

The R246 integration points were checked against the original image with SHA-256
`c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9` using Blackfin objdump:

| Location | Observed stock behavior |
|---|---|
| `0x019C2D36`, `0x019C2D3A` | Loads the complete low/high queue bytes into R0/R1, zero-extended; the synthetic tag survives. |
| `0x019C2D3E` | Calls key dispatch with those two arguments. |
| `0x019C2CF4` | Loads `0xFFA0B6E8` into P1 and calls it, with R0 = key semaphore and R1 = 0. |
| `0x019C2DCA` | Loops back to the semaphore wait after key, encoder and deferred-job processing. |

The new wait hook replaces the 10-byte load/call sequence with a call and three NOPs. The builder verifies the
original bytes and replacement length. Both hook sites are in plain LDR blocks; `l1_patches.json` is unchanged.
The VDK timeout convention is documented in the
[Analog Devices VDK guide](https://www.analog.com/media/en/dsp-documentation/software-manuals/50_vdk_mn_rev_3.5.pdf),
and the flag value is defined in [VDK_API.h](https://github.com/analogdevicesinc/VDK/blob/master/Platform/common/VDK_API.h).

## Host tests

From `synth_sdk/`:

```sh
mkdir -p harness/build
cc -std=c99 -Wall -Wextra -Werror -DOP1_HOST -fsanitize=address,undefined -g \
  harness/debounce_test.c input/debounce.c input/key_input.c -o harness/build/debounce_test
harness/build/debounce_test
```

The tests cover press and release bounce, duplicate events, short taps, retriggering at the boundary, independent
keys, modifier ordering, all physical key codes, long holds, clock wrap, invalid codes, synthetic-event bypass,
and the wait adapter's final-release and idle behavior. The host substitutes the cycle counter, queue state and
semaphore wait; it does not execute the stock firmware or establish hardware timing.

## Build and device verification

Build using the repository's normal `make_firmware.py --opz` workflow. Before publishing regenerated patch data,
test a build on an original OP-1: single taps on a bouncing key, long holds and releases, fast repeated notes,
chords, Shift shortcuts, transport buttons, encoder presses/turns, and short MIDI/sequencer gates. Check that a
released note stops with no further input, including during busy UI/file operations. Confirm that the 5 ms window
is sufficient for the affected hardware and that the release latency feels acceptable.

After the device check, build with `--opz --release` and run the patcher repository's `build_patch.py` against the
original image and rebuilt `op1_246_custom` tree. Firmware images, unpacked stock files and build products stay
git-ignored; changes to `op1r/data/` should come from that verified build.
