# Native host-stack ABI contract

Run `python -m unittest discover -s tests -p test_hle_stack_abi.py -v` on an Apple silicon Mac. This builds the real
`runtime/hle_stack.c` and `runtime/hle_stack.S`, with a synthetic assembly caller. No game data, decoder, window,
GPU or profile is used. It also runs in the root unittest suite and skips unsupported hosts.

## Why this matters for SDK adapters

Horizon and Darwin use the same ARM64 instructions but do not place every argument identically. Horizon gives the
ninth and tenth integer arguments separate eight-byte stack slots. Darwin packs two trailing `uint32_t` arguments
into offsets 0 and 4. The runtime's generic host-stack switch copies the caller's stack slots; it cannot infer the
types of an arbitrary target function and repack them.

The negative control deliberately passes values 9 and 10 in Horizon-style slots to a Darwin function with trailing
`uint32_t` parameters. The first argument arrives as 9 and the second reads the padding at offset 4 (zero in this
fixture), rather than 10 at offset 8. This mismatch is expected and is not a claim of a new failing game path.

An adapter accepting two full-width trailing slots, then narrowing their low words, receives both values correctly.
SDK 22's AV1 transfer-memory initializer has ten parameters, including two trailing enums; an implementation must
account for their guest ABI. A local inspection of the v131072 SDK confirmed caller-SP offsets 0 and 8 for these two
parameters; no game instructions or bytes are included in the test. A successful synthetic AV1 decode alone does not
check this boundary. [Mario Kart World's independent codec backend](https://github.com/ly05010419/MacNativeGames/blob/5843ee56f3ab6911e32d61d46e96fee4ce8137de/games/mkw/runtime/codec_host.c)
also uses full-width trailing parameters for guest stack arguments; its SDK structure
layouts and signatures must not be assumed to match a different SDK.

## What the probe checks

- The eight register arguments and the two stack arguments.
- No host-stack switch versus an allocated host stack, with tracing Off, counting and first-call logging: six configurations.
- Execution takes place on the expected stack and the Darwin host-stack slot is restored after each call.
- Repeated wrapper lookup preserves function identity; a null target remains null.
- The narrow negative control and the full-width adapter behave as described in all six configurations.

This is a test-only change. It does not alter the generic bridge, add AV1 handlers, or establish movie playback
correctness. It does not cover floating-point arguments, aggregate returns or every Horizon/Darwin ABI difference.

## Validation record

MacBook Pro M3 Pro, macOS 27.0, Xcode 27.2: the focused native test passes all six configurations. The full root suite
reports 1458 tests, 169 skipped, no failures/errors. Five skips were due to missing optional fontTools in the isolated
environment; after installing fontTools 4.67.0, those five named tests were rerun and all passed. The other 164 skips
are 163 Tk-dependent tests and the opt-in real Metal trace. The game runtime and its graphics code are unchanged.
