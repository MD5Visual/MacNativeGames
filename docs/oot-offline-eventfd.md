# Offline event counters and virtual-core waits

This branch follows `fix/oot-image-write-usage`. It repairs the local wake-up service in the OoT bring-up base;
network connections remain refused. gRPC and the movie TaskQueue use these counters while offline.

The contract was checked against nxnative's MIT implementation and tests at
[`ee9b7765`](https://github.com/seedds/nxnative/tree/ee9b7765d579c46e45a7297493565e098f3bda33/dm/host)
(`hle_net.c`, `tests/t_net_eventfd.c`). The implementation here uses this host's existing virtual-core API and
retains its small polling loop instead of importing nxnative's scheduler or whole service layer.

## Corrections

- EventFdRead/Write return **0 on success**, not a byte count. A non-semaphore read returns the counter before
  clearing it; a semaphore read takes one. Previously an ordinary read returned the caller's previous buffer value.
- Socket EAGAIN is BSD **35**; guest C-library errno is Linux **11**. Set both, per thread. Fcntl's BSD O_NONBLOCK
  is **4**, not Linux 2048. Close-on-exec is retained as metadata (this host has no exec).
- Zero writes succeed without advancing the counter. UINT64_MAX is rejected before subtraction, and a full counter
  cannot overflow. Descriptor lookup/state/flags now stay under the mutex. A blocked read/write detects closure and
  generation reuse instead of consuming a newly allocated descriptor's counter.
- Select preserves its requested sets while waiting, only publishes results when returning, does not invent an
  exception event, and counts each ready bit. Poll also honors POLLRDNORM. Unknown offline descriptors still report
  no poll event, preserving the bring-up host's policy; this is not a general network poll implementation.
- Finite timeouts use the monotonic deadline, including time spent waiting to regain a virtual core. Every blocking
  sleep releases that core and regains it afterward, **outside** the event-counter mutex. A producer on the same
  virtual core can therefore run. No priority, affinity, game state or guest instruction is changed.
- C++ Poll/Fcntl exports use the local event service as the C exports already do. Diagnostics remain opt-in.

## Checks

`python -m unittest discover -s tests -p test_offline_eventfd.py -v`

The production source is compiled into a small host-only probe for both Donkey Kong Bananza and OoT configurations.
Fifteen tests cover contracts, arithmetic bounds, BSD flags, readiness, delayed Select, timeouts, descriptor reuse,
thread-local error reporting and exports. Four use a single simulated virtual-core mutex: the blocked service must
release it for the producer/consumer, and regain it without holding the event mutex. These are service tests, not a
claim of full Horizon scheduler conformance. No game files, real socket connections or windows are needed.

All 30 configuration/case combinations pass. The 15 cases also pass with AddressSanitizer and UndefinedBehaviorSanitizer.
The unchanged production source fails five independent negative controls (counter, flags, Poll, Select, delayed Select).
All three Mac targets that build offline_host.c compile (DKB, Pokopia, OoT).
The complete root suite passes: 1,477 tests, 164 skipped, no failures/errors. The combined image branch also
builds all eight shared Mac runtime targets.

An iOS syntax build first hits an existing unrelated `gethostuuid` call in `pseudo_device_id`, unavailable on iOS;
the original source has the same error. A local-only syntax copy with that call removed checks the event changes
for all three variants. That workaround is not committed. No complete iOS app or device test is claimed.

## Limits

A previous game run stopped before AV1 initialization; these are concrete platform defects, but that observation
alone does not establish their responsibility for the black screen. A new bounded game trial is needed. The separate
image-write fix addresses a different verified missing output operation. Installed TOTK is untouched.
