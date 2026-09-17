# Profiling methodology — callgrind on `-I` file mode

Repeatable recipe for producing the findings in
[profiling_results.md](profiling_results.md).
No hardware needed; works on any CF32 recording.

## Build and tools

```
make profile-rtl            # or profile-rsp; separate obj/ and *.prof binaries
```

Needs `valgrind` (+ `callgrind_annotate`, ships with it), `nc` (openbsd,
has `-d`), and something to generate/keep CF32 recordings under `run/`.
Recordings named `RATEsps_CENTERHz.cf32` carry their own `-I` metadata.

## 1. Baseline profile (DSP hot spots)

File input mode is unpaced — a 30 s file runs in ~0.4 s native, ~13 s under
callgrind — so just run the whole file:

```
valgrind --tool=callgrind --callgrind-out-file=run/callgrind.out.<tag> \
  --branch-sim=no --cache-sim=no \
  bin/rtl_multi.prof -f F1,F2 -M fm -s 16k -r 48k \
  -I run/FILE.cf32:RATE:CENTER -O /dev/null,/dev/null
```

- Ir-only (`--branch-sim=no --cache-sim=no`): Ir is deterministic, so two
  runs are comparable by **absolute** Ir (never by % across different totals).
- `-O /dev/null,/dev/null` exercises the full demod + pack path (file
  output mode, no TCP fast-path skip).
- Pick channels to match the recording's span (offsets within ±RATE/2).

## 2. Reading results

| want                          | use                                              |
| ----------------------------- | ------------------------------------------------ |
| hot spots ranking             | `callgrind_annotate run/callgrind.out.<tag>`     |
| inclusive per pipeline stage  | `callgrind_annotate --inclusive=yes ...`         |
| who calls a callee (+ counts) | `callgrind_annotate --tree=caller ...` then grep |

Trust **self Ir, call counts, and `--tree=caller` edges** — those are exact.
Two artifacts to not fight:

- `???` file prefixes: our functions carry no source-file info in the
  profile, so per-line auto-annotation of `dsp.c` is unavailable. Function
  granularity + reading the source is enough; don't chase it.
- `'2`-suffixed names and inclusive numbers that exceed 100% or contradict
  each other around init functions (`firdes_kaiser` vs `resamp_rrrf_create`
  overlap): context-splitting. Reconstruct init costs by **summing self Ir
  of the known design-chain functions** instead of trusting inclusive.

### Per-sample arithmetic

Derive iteration counts from rates × duration × channels, then divide:

- input iterations/chunk-stage = chunks × 8192 × channel_count
  (chunks = file_samples / 8192, printed chunk size 8192)
- demod samples = channel_rate × duration × channels (16 kS/s here)
- audio outputs = output_rate × duration × channels (48 kS/s here)

Ir/call for library functions = self Ir / measured call count.

### Init vs hot path

Classify by call count heuristic: counts ≈ chunks/samples/outputs are hot
path; counts ≈ tap counts, channel counts, or 1 are one-time init
(filter design, LUT build, global memsets). `logf`/`log`/`expf` called
~1M times here are still init — they hang off liquid's Kaiser/Bessel
design chain, not the streaming path. Sum init selfs separately from stage
selfs or the stage table won't balance.

### Library split one-liner

```
callgrind_annotate --threshold=100 out.file | \
  grep -E "^\s*[0-9,]+ \(\s*[0-9.]+%\)" | grep -v "PROGRAM TOTALS" | awk '{
  ir=$1; gsub(",","",ir); lib="rxx_multi";
  if ($0 ~ /libliquid/) lib="libliquid"; else if ($0 ~ /libm\.so/) lib="libm";
  else if ($0 ~ /libc\.so/) lib="libc"; else if ($0 ~ /ld-linux/) lib="ld.so";
  sum[lib]+=ir; tot+=ir } END { for (l in sum) printf "%-10s %14d %6.2f%%\n",
  l, sum[l], 100*sum[l]/tot }' | sort -k2 -rn
```

## 3. TCP variant (output path active)

Clients cannot be spawned _before_ the profiled process (nothing is
listening yet), and don't need to be: under callgrind the run lasts many
seconds, so launch first, poll for the ports, then connect. The `-d` flag
keeps `nc` from exiting on stdin EOF; redirect stdout to discard.

```bash
#!/bin/bash
valgrind --tool=callgrind --callgrind-out-file=run/callgrind.out.<tag>_tcp \
  --branch-sim=no --cache-sim=no \
  bin/rtl_multi.prof -f F1,F2 -M fm -s 16k -r 48k \
  -I run/FILE.cf32:RATE:CENTER -O tcp:8101,tcp:8102 2>/tmp/opencode/run.log &
VPID=$!
for port in 8101 8102; do
  until (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null; do sleep 0.05; done
done
nc -d 127.0.0.1 8101 > /dev/null 2>&1 & NC1=$!
nc -d 127.0.0.1 8102 > /dev/null 2>&1 & NC2=$!
wait $VPID; kill $NC1 $NC2 2>/dev/null
grep -c "connected from" /tmp/opencode/run.log   # verify clients attached
```

- The port probe itself connects and occupies a client slot until the first
  `send()` drops it (`TCP: client [0] lost` in the log) — harmless, the
  real client holds the second slot; verify via the app log, not assumptions.
- Compare against the baseline by absolute Ir delta. The DSP portion is
  bit-identical between runs; the delta is the output path.
- **Syscall caveat:** valgrind passes syscalls through, so kernel-side
  `send`/`futex`/`write` cost is invisible. A "+0.13%" TCP result means
  user-space Ir only, not wall-clock truth.

## Pitfalls (things that waste time)

- `valgrind --tool=dhat` **hangs** on this multithreaded app (precedent:
  funcs.md notes, 2026-09-16). Use callgrind for DSP questions and
  `strace -c -f` / `ltrace -c -f` for syscall/allocation counts.
- Don't write ad-hoc callgrind-format parsers; callgrind_annotate covers
  self costs, call counts, and caller edges, and its self totals sum to
  exactly 100%. Ad-hoc parsers will double-count across context splits.
- Don't raw-`diff` extracted audio against `run/*.raw` references to
  validate flags — those are reference _audio_ from a different build for
  by-ear regression, not bit-exact fixtures.
- `-M wbfm` silently changes rate/deemp globals; use explicit `-M fm -s ..
-r ..` when profiling NBFM so the rate chain stays where you put it.
- File mode has no pacing and no DC-cal drop (that's RSP-specific); don't
  expect RTL and RSP profiles to differ in `-I` mode — they shouldn't.
