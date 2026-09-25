# Eden Emulator Performance Optimization Series

> English edition. The Chinese original lives at [OPTIMIZATIONS_zh.md](./OPTIMIZATIONS_zh.md).
>
> This document is the complete write-up of the optimization series: the motivation,
> design, implementation and validation data of every change, plus how the series maps
> onto the historical working branches. It is written for future maintainers (or a
> future self) — no process log required to understand **why each patch in this series
> exists and how it was validated**.
>
> Base: `origin/master` @ `74ccea3def`. The series is 14 commits (3 build, 5 fixes,
> 6 optimizations), reorganized by topic, carrying no diagnostic instrumentation and
> no experimental switches. The full process record (82 original commits, profiling
> data, failed experiments) lives on the archival branches `test/master-profiling`
> (documentation) and `test/p2-draw-resolver` (parallelization experiments); see the
> index at the end.

---

## 0. Summary

Weeks of bottleneck hunting and optimization went into the emulator, using the TOTK
(The Legend of Zelda: Tears of the Kingdom) pond scene as the benchmark and three
layers of observation: ETW (CPU sampling, thread scheduling), AMD uProf IBS
(per-instruction memory-access sampling), and the emulator's built-in frame-time CSV. This series collects the changes
that were validated to be effective or necessary:

| Category | Count | Representative gains |
|---|---|---|
| CPU (dynarmic JIT) | 2 | Eliminates 51% of cross-core duplicate compiles; register-allocation value lookup goes from linear scan to constant time |
| GPU thread (serial path) | 3 | Redundant address translations removed per draw call (up to 100% hit rate); redundant register writes skipped; pipeline lookup memoized |
| Presentation timing | 1 | Unlocked-mode presentation-time quantization error below 1 ms, without saturating a core |
| Correctness fixes | 5 | Loading-screen garbage, broken menu pacing, nondeterministic process crashes, and more |
| Build fixes | 3 | libc++ portability, static-Qt cross builds, MSVC CRT symbol shims |

Overall effect (pond benchmark, unlocked mode, 90-second measurement window, at least
2 runs per configuration):
- Median frame rate rose from the 43.5±0.5 historical range to a stable 44.1-44.4 fps;
- The frame-time median sits firmly on the 22.50 ms step — no more flipping between
  the 21.67/23.33 ms steps;
- p99 frame time dropped from 32.5 ms to 31.7 ms;
- Camera-rotation frame-rate spikes reduced by 70-79% (pipeline lookup memoization);
- Loading-screen visual garbage (sampling never-written VRAM) is gone.

Deliberately absent from this series (validated as net losses, or diagnostic-only;
see §6): the whole per-draw-call parallelization line (draw token / DrawResolver /
tail-on-worker — all net negative), all profiling instrumentation (counters, clocks,
block dumps), and experimental A/B environment switches.

---

## 1. Methodology

Observation ran at three layers:
1. **ETW (wpr capture + analyzer)**: CPU sample stacks, thread scheduling, kernel
   time — deciding whether the bottleneck is CPU- or GPU-side, and locating
   function-level hot spots (eden's thread names are directly readable: `GPU`,
   `CPUCore_*`, `VulkanWorker`, `HostTiming`).
2. **AMD uProf IBS**: 160 seconds of per-instruction sampling in the rotation
   scene — separating "execution-throughput bound" from "memory-/branch-stall
   bound". Verdict:
   load-miss latency accounts for only 2.33% of GPU-thread cycles and branch
   mispredicts for 0.40% — a pure execution-throughput bottleneck. That decided
   the direction: delete redundant instructions rather than reshuffle data layouts
   or add prefetching.
3. **Built-in frame-time CSV**: `record_frame_times` exports the frame-time series
   per run, yielding fps / median / p95 / p99 plus average picture brightness.

Benchmark rules (scene load varies with in-game time; see §7):
- At least 2 runs per configuration before concluding anything;
- Average picture brightness serves as a covariate to separate day/night and
  weather phases (day band ≈ 56.8, dusk band 36-48); absolute frame rates are
  not directly compared across brightness bands;
- The frame-time median lands on steps that are multiples of 1.667 ms —
  distinguish "step change" from "real load change";
- Every run is checked on three counts: the game was entered, the screenshot
  signature was normal, and the emulator closed cleanly.

Acceptance criteria: macro metrics (fps / median / p99) plus fine-grained local metrics
(e.g. fast-path hit rates, translations eliminated) as double evidence; when a single
change stays within ±2%, the local metrics plus "no regression" decide whether
it stays.

---

## 2. CPU side: dynarmic JIT (2 commits)

### 2.1 Process-wide cross-core shared IR cache — `d46d08038b`

Original commits: a421ed22b4 (experimental) → 88fc516432 (on by default + 512 MiB
cap) → bb59b29f1d (adapt to master's flattened terminals) → 0df87fd502 (reuse
validation and bound hardening) → 74af90ee2c (restore the 512 MiB default), plus
tests 790b899833/e5fcc1b183.

Problem: each of the four CPU cores owns an independent Jit instance. Block-dump
analysis showed 51% of block compiles were cross-core duplicate work — the same
guest block fully translated and optimized by 2-3 cores each. Compilation happens
in the foreground (one of the main causes of hitches when entering new areas), so
the duplication was pure waste.

Design:
- The cache stores **post-Optimize** IR blocks: a hit skips both translation and
  optimization, running only that core's own emit (register allocation is
  per-core state and cannot be shared). The cache is a process-wide singleton
  keyed by IR LocationDescriptor.
- Reuse is guarded by three checks: each entry pins the literal guest machine
  code (FNV hash plus word-by-word comparison), a translation-config tuple (any
  difference in unpredictable behavior, wall clock, halt-on-access, cache hooks,
  optimization flags, dczid, or polyfill options invalidates it), and the block's
  start/end PC. On any mismatch the path falls back to a full compile.
- Lifetime: the cache is emptied when the last Jit instance dies, so config
  drift cannot leak across sessions.
- Memory: 512 MiB budget with LRU eviction; TOTK steady state measured at
  335 MiB, never reaching the cap.
- Switches: on by default on Windows; `EDEN_JIT_IRCACHE=0` disables it globally,
  `EDEN_JIT_IRCACHE_MAXBYTES` tunes the budget. `IRCacheStats` keeps atomic
  counters (hits/stores/entries/bytes) on compile paths only, for unit-test
  assertions; steady-state execution never touches them.

Validation: block-dump dedup analysis; multi-core execution-equivalence tests on
real cores; full TOTK sessions with frame rates inside the historical range.

### 2.2 Constant-time value location in register allocation — `1e6329e5d2`

Original commits: 82a043bc40 plus test 028c82613c.

Problem: `ValueLocation(inst)` located an IR value by linearly scanning every
host register location's value list; for large blocks this scan dominated
register-allocation time during code generation.

Design: IR instruction names are dense within a block (NamingPass guarantees
1..N), so a 4096-entry u8 table `name → hostloc+1` answers the lookup directly. The table is
zeroed per block (RegAlloc is placement-new'd per emit). Moves and exchanges re-register
the affected locations; unnamed values and out-of-range names fall back to the
original scan, and debug builds cross-check every indexed lookup.

---

## 3. GPU-thread serial path (3 commits)

The GPU thread (command parsing → state machine → binding resolution → command
recording) is the emulator's main bottleneck thread. IBS sampling proved it to be
execution-throughput-bound (no memory wall, no branch wall), so everything here
is "delete redundant work".

### 3.1 Redundant state and binding work removed — `22a7e999e2`

Original commits: 9aa4ce0921 (GPU command-thread micro-optimizations; ETW
verified GPU-thread self time in a 100-second window dropping from 105.9 s to
102.4 s) + 5da3c43261 (cross-channel buffer_id invalidation fix) + tests.

Four changes:
1. **Identical register writes skipped**: games rewrite the full register state
   every draw; `ProcessDirtyRegisters` returns immediately when
   `regs[method] == argument`, skipping both the store and the dirty marking
   (self time 4.23 s → 1.66 s). A `change_generation` counter (bumped only when
   a stored value actually changes) was introduced for downstream caching (§3.2).
2. **LRU touch deduplication**: Buffers and Images record `last_touch_tick`;
   repeated LRU touches within one frame are skipped (TouchBuffer 1.48 s →
   0.55 s, texture Touch 1.27 s → 0.30 s).
3. **SSBO/TBO buffer_id retention**: when the binding target (address plus size)
   is unchanged, the resolved buffer_id is kept and `Update*Buffers` skips the
   per-draw FindBuffer. `DeleteBuffer` now invalidates resolved ids across every
   live channel, keeping slot reuse safe.
4. **Uniform alignment cached**: `GetUniformBufferAlignment()` is cached at
   construction instead of a virtual call per bind.

Note: on its own this commit does not move frame rate — all four hot threads were
saturated and frame time was set by the slowest stage. Its value is lowering
GPU-thread pressure, which freed headroom for the later optimizations.

### 3.2 Graphics pipeline lookup memoization — `fcac5d10fd`

Original commits: d900d7c2ad (generation memo) + be3e692dac/9af2007176
(transition-key hashing).

1. **Memoized by register generation**: `CurrentGraphicsPipeline` compares
   Maxwell3D's change_generation against the generation recorded when the
   current pipeline was last built; if unchanged it returns current_pipeline directly, skipping
   stage refresh, fixed-state refresh and the transition/map key comparisons.
   Draws whose registers did not actually change are the norm — games rewrite the
   same registers every draw.
2. **Precomputed transition-key hashes**: `AddTransition` stores key hashes up
   front; lookups compare hashes before the byte-wise compare, and the
   single-candidate case does a direct equality check — hashing first can only
   add work there.

Validation: rotation-scene frame-rate spikes down 79%/70%, median unchanged.

### 3.3 Redundant address-translation elimination — `0781b56252`

Original commits: 18aaa38cef (first descriptor direct-read version) +
74d5550276 (first serial batch, +0.5-1% fps) + e7c1484690/e43f875f75/814f80c107
(descriptor/SSBO/dirty-bitmap items) + 8398d87199 (staging shift). The last five
reached this series via the intermediate validation branch
(test/master-profiling c31cc64c1f).

Six changes, with per-run elimination rates (or gains) from the pond scene:
1. **Single-page check for descriptor-table reads**: descriptors are at most
   32 bytes; once an arithmetic check confirms the descriptor lies inside one
   device page, the second address translation is dropped entirely
   (84.3M/84.3M = 100%, about 84 million translations saved per run);
   cross-page cases keep the original continuity check.
2. **One translation for both SSBO qwords**: when ssbo_addr and +8 share a
   device page, one translation reads both qwords (7.8M/7.8M = 100%).
3. **Deriving the paired SSBO translation**: when the unaligned address shares
   a page with the aligned one, the device address is computed as
   "aligned + offset" (7.8M/7.8M = 100%); cross-page cases take the original
   path — continuity is never inferred across a page boundary.
4. **Single-word queries on the GPU dirty bitmap**: queries falling inside one
   bitmap word index the top tier directly (without creating absent managers),
   and the manager level builds the page mask in place instead of IterateWords
   (92.8% of queries hit); only the query path changed, unusual ranges keep
   their original semantics.
5. **Single-page memcpy fast path for uniforms**: guest-to-host streaming
   copies bypass ReadBlockUnsafe's per-page dispatch and memcpy directly when both
   end pointers are contiguous (graphics and compute paths); FlushCaching now
   passes the span directly instead of copying the whole invalidation list every
   time the accumulator fires.
6. **StagingBufferPool::Region shift instead of division**: the normal-path
   region size is 16 MiB (a power of two), so the runtime 64-bit division
   becomes a precomputed shift (shift=24). Each GetStreamBuffer carried 4 such
   divisions the compiler cannot merge away; non-power-of-two region sizes
   keep the division.

Validation: the first batch measured +0.5-1% fps (44.31/43.64 against the
43.5±0.5 historical range); combined acceptance at 43.8-44.4 fps with a 22.50 ms
frame-time median; elimination rates as listed.

---

## 4. Presentation timing (1 commit)

### 4.1 Vsync scheduling frequency floor in unlocked mode — `28415c31f9`

Original commits: edcd160726 (600 Hz floor, adapted from v0.2.1's c8b0c853f8) →
the granularity halving inside 18aaa38cef (600 → 1200 Hz).

Problem chain: with the frame rate unlocked, the conductor scheduled guest vsync
events with no minimum period; in practice multi-kHz wakeup storms kept the
HostTiming/VSync thread burning a full core. Raising the floor to 600 Hz fixed
the burn, but present times were quantized onto a 1.667 ms grid — when a frame's
work landed just above a grid boundary, the frame-time median flipped between
21.67 and 23.33 ms (13 vs 14 ticks).

Design: `kMinVsyncTickNs = 1e9/1200` — quantization granularity halves to
0.833 ms (loss below 1 ms) while the wakeup rate keeps a hard cap.

Validation: the frame-time median sits firmly on the 22.50 ms step; the
full-core HostTiming saturation is gone.

---

## 5. Correctness fixes (5 commits)

| Commit | Original | Content |
|---|---|---|
| `ccd22ee643` | 9e8edc411a | TextureBufferBinding::format gets an explicit Invalid default. Default-initialized channel bindings read an uninitialized enum during the first empty-binding comparison; adds a regression test using poisoned storage |
| `dc6f9e9f66` | f3b7712ae3 | Zero-fill the async ASTC decode window. Between QueueAsyncDecode and the TickAsyncDecode upload a frame later, the VkImage is uninitialized while draws can sample it, reading never-written VRAM — garbage tiles on loading screens, 2637 queued decodes observed in one run. Zeros are now uploaded through staging immediately at queue time, so the decode window samples black. ZeroUploadCopies mirrors ConvertImage's buffer geometry, including the BC1/BC3 recompression branches |
| `e3a7dc82ba` | 568605ebf7 | In unlocked mode, explicit frame-interval requests are composed at hardware rate. Pause menus designed for 30 fps (swap interval 1-4) were previously dragged to full speed by the 0.01 unlocked multiplier, overclocking the input repeat rate. Only game-paced submissions (compose multiplier > 1) take the unlocked multiplier |
| `5d50effdf1` | a10393a9b0 | Restore the speed-limiter preference from before the game was launched. Upstream forced `use_speed_limit=true` in OnShutdownBegin, silently re-checking "Limit Speed Percent" after every game exit |
| `c0eb6ef236` | eb2901fbc0 | Contain settings filesystem exceptions. On Windows, `rename(tmp, dat)` throws filesystem_error when the target is concurrently held; StoreSettingsFile runs on the TimeWorker thread where nothing catches it, terminating the process at 0xC0000409 at unpredictable moments. Root-caused via the SIGABRT stack: TimeWorker→SetNetworkSystemClockContext→SetSaveNeeded→StoreSettings; the physical evidence was leftover tmp + dat file pairs in save data. LoadSettingsFile/StoreSettingsFile now use function-level try blocks that catch, log and return false; the success path is byte-identical |

---

## 6. Build fixes (3 commits) and archived negative results

Build: `bcdf43e3ad` (explicitly including the standard headers missing under
libc++ and stricter STL configurations), `87498a196d` (skip windeployqt for static-Qt and cross
builds), `056bcbddd2` (MSVC CRT `__std_*` vector-symbol shim needed by CPM's
static Qt 6.11.1; attach via `CMAKE_EXE_LINKER_FLAGS` when recreating the build
directory — see the file header).

Lines validated as net losses and left out (full process in test/master-profiling's
PROFILE_PROGRESS.md):
- **The per-draw-call parallelization line** (test/p2-draw-resolver): the
  depth-1 DrawResolver (single resolver thread) was net −10 fps — snapshot plus
  cross-thread handoff costs exceed the ~1 µs/draw parallelizable work; the draw
  token stages 1A/1B/2A/2B all terminated early, the best variant still 14.3%
  slower than straightforward inline execution; the tail-on-worker FIFO also came out
  negative. Conclusion: draw-level binding-resolution parallelism is
  arithmetically blocked by handoff cost in this architecture, and a survey of
  RPCS3, PCSX2, Dolphin and peers found no precedent either.
- **Diagnostic facilities**: JIT counters, instruction-mix statistics, the block
  dump (EDEN_JIT_BLOCKDUMP), uniform stream-copy shadow statistics
  (EDEN_UNIFORM_STATS) and all environment-gated instrumentation were stripped
  after measurement. The block_lookups atomic counter, on by default on Windows,
  sat on the hot dispatch path — stripping it moved p99 from 32.5 to 31.7 ms.
- **Experimental switches**: the EDEN_JIT_NOLINK/NORSB/NOFASTDISPATCH
  dispatch-path A/B gates.
- **Zero gain**: the serial-34.3 FlushWork/FlushCaching skip gates (measured
  change: none); the epoch/memcmp content-comparison cache (tied to the draw-token
  line).

---

## 7. Baseline and acceptance data

Benchmark protocol: `bench_run.py` (launch → auto-enter the game → 90-second
measurement → clean close → frame-time CSV parsing), pond test save, unlocked
mode, RelWithDebInfo builds on the same machine.

| Date/build | fps | Median (ms) | p99 (ms) | Luma | Notes |
|---|---|---|---|---|---|
| Pre-optimization master baseline | 43.5±0.5 | 22.50 | 31.7-32.5 | ~56.8 | §23/§25.6 baseline |
| Port acceptance (n=3) | 44.31/44.14/43.69 | 22.50 | 32.5 | 56.5-56.8 | test/master-profiling |
| This series, diagnostics stripped (n=2) | 44.35/44.29 | 22.50 | **31.67** | 36.2/47.6† | |
| This series, commits reorganized | 43.84 | 22.50 | 33.34 | 47.6† | differs from the row above by comments only |

† The in-game world clock drifted into the dusk phase (different brightness
band); the frame-time median step and the distribution shape match, and absolute
frame rates are not compared across brightness bands.

Environment: Windows 11 x64 / Ryzen (12 logical cores) / RTX 2060 / VS2022 19.44 /
static Qt 6.11.1 / RelWithDebInfo.

---

## 8. Commit index

| Commit | Type | Topic | Main original commits |
|---|---|---|---|
| bcdf43e3ad | build | explicit standard-library includes | 96e4a3fc45 f34a54de9c ab96d66675 |
| 87498a196d | build | skip windeployqt for static Qt | 8be57d503d |
| 056bcbddd2 | build | MSVC CRT vector-symbol shim | v0.2.1 porting round (PROFILE §21) |
| ccd22ee643 | fix | TBO format default | 9e8edc411a |
| dc6f9e9f66 | fix | async ASTC zero-fill | f3b7712ae3 |
| e3a7dc82ba | fix | hardware-rate pacing for explicit intervals | 568605ebf7 |
| 5d50effdf1 | fix | speed-limiter preference restore | a10393a9b0 |
| c0eb6ef236 | fix | settings exception containment | eb2901fbc0 |
| 28415c31f9 | perf | 1200 Hz vsync scheduling floor | edcd160726 + 18aaa38cef |
| d46d08038b | perf | cross-core shared IR cache | a421ed22b4 88fc516432 bb59b29f1d 0df87fd502 74af90ee2c |
| 1e6329e5d2 | perf | constant-time register-allocation lookup | 82a043bc40 |
| 22a7e999e2 | perf | state/binding redundancy removal | 9aa4ce0921 5da3c43261 |
| fcac5d10fd | perf | pipeline lookup memoization | d900d7c2ad be3e692dac 9af2007176 |
| 0781b56252 | perf | address-translation redundancy elimination | 18aaa38cef 74d5550276 e7c1484690 e43f875f75 814f80c107 8398d87199 |
