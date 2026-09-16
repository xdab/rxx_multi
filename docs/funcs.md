# Syscall / allocation / memcpy profile — `-I` replay, 2ch FM

Baseline workload (`run/429MHz_2Msps.cf32`, ~28 s audio, 2 MS/s, center 429 M):

```
bin/rsp_multi -I run/429MHz_2Msps.cf32:2M:429M \
  -f 428.775M,429.525M -M fm,fm \
  -O /tmp/a.raw,/tmp/b.raw [-x N]
```

## Syscalls (strace -c -f, full file, 6 threads)

| syscall            | calls  | % time | notes                                          |
| ------------------ | ------ | ------ | ---------------------------------------------- |
| `futex`            | 54 596 | 51 %   | condvar/mutex handoffs; 4 363 benign EAGAINs   |
| `clock_nanosleep`  | 12 718 | 41 %   | spin-wait pacing, see below                    |
| `read`             | 6 861  | 7 %    | CF32 input, ~65 KB per read                    |
| `write`            | 1 334  | <1 %   | PCM16 file output                              |
| rest (startup)     | ~180   | ~0 %   | mmap, clone3 x5, openat, ...                   |
| **total**          | 75 689 |        |                                                |

## clock_nanosleep attribution (strace -ff, `-x 3`)

All sleeps come from spin-wait loops; no timed condvar waits in the code.
TIDs: main 29171, output ch0/ch1 29173/29174, demod ch0/ch1 29175/29176,
file input 29177 (creation order in `supervise()`).

| thread          | calls (3 s) | source                                        |
| --------------- | ----------- | --------------------------------------------- |
| file input      | 7 159       | `usleep(50)` polling in `acquire_fill_slot()` |
|                 |             | `src/backend/rsp/thread_device.c:62`          |
| demod ch0/ch1   | 2 911/2 961 | `usleep(100)` waiting for output stage        |
|                 |             | `src/core/thread_demod.c:93`                  |
| main            | 17          | supervisor `usleep(100000)` `main.c:336`      |
| output ch0/ch1  | 0           | sits in `pthread_cond_wait` (futex)           |

Hardware-driven runs pace the producer at real sample arrival, so the
file-input spin is a `-I` mode artifact; the demod→output spin scales with
output backpressure.

## malloc / memcpy (ltrace -c -f, `-x 2`)

| function | calls | notes                                        |
| -------- | ----- | -------------------------------------------- |
| `memcpy` | 4 800 | ~2 400/s ≈ 244 chunks/s x 5 sites x 2 demods |
| `malloc` | 4     | startup only (liquid lazy init)              |
| `free`   | 4     | matching                                     |
| `realloc`/`calloc`/`memmove` | 0 |                              |

- **No dynamic allocation in the hot path** — confirmed: 4 mallocs total.
- memcpy sites are the 5 in `src/core/dsp.c:303-384` (decimator tail
  upkeep x3, post-decimation copy, shift output copy) at ~244 chunks/s
  per channel. Extrapolated to the full 28 s file: ~67 k memcpys.

## Tool notes

- `strace` / `ltrace` (ptrace): fine for this workload, counts exact.
- `valgrind --tool=dhat`: **hangs** with this multithreaded app (stuck in
  valgrind syscall dispatch, 2/6 threads, 0 % CPU) — killed; don't retry
  without a single-thread repro.
- `callgrind`: works on this app (`run/callgrind.out.*` precedents) but
  ~40x slowdown; too heavy for syscall-level counting, use for DSP
  function profiling instead.

## 2026-09-16: spin loops -> condvars

Replaced both `usleep` polling loops with blocking condvar waits
(commit-style summary, validated on this same workload):

- demod -> output handoff (`thread_demod.c`): waits on
  `output_state.written` while `seq_written != seq_packed`; output
  broadcasts after every `seq_written++`.
- producer refill bar / EOF drain (`acquire_fill_slot` /
  `wait_demods_drained`, both backends): wait on
  `device_state.slots_drained`; demods broadcast after every
  `seq_processed++` (including the TCP no-client fast-path bump).

The ping-pong invariant triangle is unchanged: index-based consumption,
refill-barred-until-consumed, counter-level predicates. Only the wakeup
mechanism changed; wakeups are now data-driven instead of timer-driven.

### Results (same workload, same tooling)

| metric                    | before          | after           |
| ------------------------- | --------------- | --------------- |
| `clock_nanosleep`         | 12 718          | 9               |
| `futex`                   | 54 596          | 94 588          |
| total syscall time (strace-inflated) | 2.97 s | 2.58 s    |
| full-file pipeline wall time | 0.69 s       | 0.39 s (+43 %)  |
| `malloc` (whole run)      | 4               | 4               |

- futex count rose as expected: each handoff is now one blocking
  wait/wake pair instead of a poll quantum; uncontended futex ops are
  ~1 us and eliminate the up-to-100 us poll-quantum latency, which is
  where the 43 % throughput gain comes from.
- Functional regression check: full-file replay output (both channels)
  is **bit-identical** to the pre-change binary (verified deterministic
  baseline via git worktree build, `cmp`).
- Shutdown paths re-verified: `-x` timeout, SIGINT mid-run, RTL backend
  `-I` full run — all exit cleanly (exit 0). A 0-byte output file when
  SIGINT lands during pipeline startup (< ~0.2 s) is pre-existing
  behavior, identical on the old binary.
