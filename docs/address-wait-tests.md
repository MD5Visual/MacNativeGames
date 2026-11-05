# Address arbitration tests

`python -m unittest discover -s tests -p test_address_wait.py -v` compiles `address_wait_probe.c` twice: the base runtime
and `GAME_OOT` / `GAME_NS2`. Both include the actual `runtime/threads.c` dispatcher, atomic accessors, address queue,
priority/FIFO selection and pthread wait/wake implementation. The fixture supplies synthetic words and host-thread
records and disables virtual-core scheduling. It does not load game data, run guest code, use the GPU or touch a profile.

Coverage includes full-width X2 comparisons (including high-bit-only mismatches, bit 63 and all ones), zero and finite
timeouts, waking by the documented type-0 signal, address isolation, positive/zero/negative wake counts, priority and
FIFO selection, mixed-width waiters, changing a word without signalling, 32-bit signed comparisons/decrements and
low-word-only updates. Four-byte alignment is accepted for the 32-bit modes. Deliberately mutated builds truncate X2
or load only 32 bits; the regression checks detect both errors. Calls also check that the saved guest register slots
X1..X31 are preserved; the fixture invokes the C dispatcher, not a rewritten SVC gate.

## Validation regressions from 2e7e6036

The original tests recorded two explicit `expectedFailure` cases on the bring-up branch:

- A 64-bit wait on a four-byte-aligned but not eight-byte-aligned address should return InvalidAddress (kernel module
  1, description 102, `0xcc01`). The implementation reads it without that validation. On the tested M3 Pro it returns
  TimedOut instead. A fault on another host is caught inside the probe process, without involving the game.
- An unrecognised signal type (4) should return InvalidEnumValue (description 120, `0xf001`) and preserve the word.
  The implementation instead enters its 32-bit increment path, can return success and change the word.

Both are fixed by checking alignment and the operation type before accessing the word or the wait queue, and both
markers have been removed. Additional cases cover legacy 32-bit alignment, invalid wait types, both signal widths,
and PROT_NONE memory: invalid parameters must return before any load or store. The fork's existing signal type 3 is
preserved pending clarification of its API contract. These fixes do not establish a cause for OoT's render-pass gate.

## API scope

[libnx svc.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h) declares WaitIfEqual64
and an `s64` value for WaitForAddress, but only SignalType 0..2 with an `s32` value for SignalToAddress.
[The public address-arbiter specification](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_address_arbiter.cpp)
distinguishes eight-byte alignment for the 64-bit wait and four-byte alignment for other waits and signals.
[Result definitions](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/results/svc_results.hpp)
identify the two error numbers above. These are behavioural references; no code from those implementations is copied.

The fork's signal type 3 (64-bit compare/increment) is separately labelled a characterization test, including a
low-word carry, wraparound and high-word mismatch. It is not a claim that Switch hardware implements that operation.
The documented 64-bit-wait / type-0-signal path is tested independently. A game trace or an SDK reference is still
needed to establish the intended contract of signal type 3.
