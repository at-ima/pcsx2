# Intro performance investigation (2026-09-18)

## Scope and controls

The test is the opening movie of Saru! Get You! 2, SCPS-15025, CRC FE0A6AB6,
on an Apple M5 with 32 GB RAM. Source baseline: `98b0f5267` (runtime changes
through `e0416704b`), Release ARM64, LTO disabled. Temporary diagnostics were
removed after measurement. The executable's cached version string still names
`8ee37fa82`; that string must not be used to identify these experiments.

Runs fast-boot from the disc, without a save state, using separate test data
directories and the same BIOS/settings. MTVU is explicitly disabled. The nominal
speed limit is 100%, renderer selection is automatic, resolution is native, and
GameDB fixes and patches are enabled unless stated otherwise. No builds or other
benchmark runs overlap the measurements.

Performance comparisons select completed metric samples within emulated frames
850–1100. Throughput is the difference in frame numbers divided by elapsed log
time between the first and last selected sample, not an average of instantaneous
FPS. This is vertical-frame throughput (VPS); internal FPS also tracks VPS in
this interval. CPU/GS figures below are their separate thread CPU times and must
not be added as sequential frame latency.

The installed Rosetta application is **2.8.2**, whereas this fork is based on
2.9.58. Earlier test settings also requested MTVU, which the ARM64 backend cannot
use; this option must be controlled explicitly in an x64 comparison. No controlled
x64/ARM64 speed ratio was established here. A same-revision,
same-settings x64 measurement remains necessary before attributing the entire
reported difference to the architecture. The user's approximately 15 FPS was
not reproduced by this dedicated configuration.

## Controlled ARM64 measurements

| Condition | VPS | CPU thread ms/frame | GS thread ms/frame |
| --- | ---: | ---: | ---: |
| Baseline, first run | 24.70 | 40.32 | 3.42 |
| Disable GameDB game fixes | 25.08 | 39.75 | 2.75 |
| Disable EE native backend | 22.61 | 44.15 | 3.37 |
| Disable VU1 native backend | 11.55 | 86.42 | 3.16 |
| Baseline, repeat | 24.86 | 40.14 | 3.38 |

This game's logged GameDB game fix is SoftwareRendererFMV. Disabling game fixes
removes its renderer switches but does not disable game patches. The small
throughput change does not explain the large performance deficit. CPU time
approximately fills the frame interval even with the normal software-renderer
switch, indicating CPU-thread saturation rather than a primary GS-thread limit.

The VU1 native backend already provides about 2.1x total throughput relative to
the VU1 interpreter in this scene. Disabling EE native execution is also slower.
These are whole-emulator comparisons, not isolated backend speedups or direct
measurements of each subsystem's share of CPU time.

## CPU sampling

A separate five-second `sample` capture begins after the first metric at frame
900. It contains 2,985 CPU-thread samples and excludes shutdown. Sampling itself
reduces throughput, so this run is excluded from the performance table.

Counts below are mutually exclusive: each call-tree node contributes its count
minus its immediate children's counts. Generated-code addresses are classified
using this run's logged cache ranges, not addresses from another process. The
shared VU pipeline occupies offsets `0..0x52f`, confirmed by disassembly.

| CPU work | Samples | Share |
| --- | ---: | ---: |
| VU1 shared native pipeline, self | 644 | 21.6% |
| VU1 generated blocks, self | 535 | 17.9% |
| XGKICK/GIF, including callees | 366 | 12.3% |
| EE dispatcher/native calls, including callees | 366 | 12.3% |
| EE generated code without dispatcher ancestry | 164 | 5.5% |
| IOP interpreter/events, including callees | 332 | 11.1% |
| VU1 interpreter fallback, including callees | 78 | 2.6% |
| VU1 dispatcher, remaining callees | 65 | 2.2% |
| Other CPU work | 435 | 14.6% |

JIT unwinding is incomplete. In particular, generated-block self time includes
control, flag, and boundary work, not just arithmetic. These approximate shares
describe this short window, not the whole game. Hot shared-pipeline locations
include FMAC latency loads, queue-count stores, and retirement return paths.

## Separate execution-count run

Counters were collected from boot until frame 1104 in another build. They are
not restricted to frames 850–1100, and counter overhead makes their FPS unsuitable
for the performance table.

- VU1: 44,337,942 native block calls, 965,465,366 native pairs, averaging **21.78
  pairs/call**. Native cycles equal native pairs in this run. There are also
  20,925,338 unsupported fallback pairs and 3,985,686 pending-delay pairs.
- EE: 422,799,639 dispatch attempts, 269,479,101 generated-function calls,
  1,334,572,220 completed instructions: **4.95 instructions/native call**.
  23,567,186 calls (8.75%) exit with a zero-length prefix. These counts are not
  an EE instruction-coverage percentage; interpreter branch delay slots are not
  included in the completed-native count.
- XGKICK: 124,045,824 calls submitting chunks to GIF, totaling 2,104,295,424
  logical bytes. **99.80% submit exactly 16 bytes**; mean size is 16.96 bytes.
  There are 3,985,408 tag-size queries, or 31.125 chunk submissions per query.

The transfer granularity is therefore an observed workload property, not merely
an inference from source code. Each actual transfer also crosses the native/C++
boundary and publishes/reloads cached VU vectors. Meanwhile, native VU blocks
are reasonably long: assuming that most calls execute tiny prefixes would be
incorrect for this workload.

## Implications and next experiments

1. Investigate reducing repeated pipeline work across a native block. Per-pair
   architectural queue retirement remains expensive even when the executed pairs
   do not stall. x64 microVU's compile-time scheduling is the relevant reference;
   precise fallback, budget exits, flags, and cycle-wrap behavior must remain
   correct. Removing budget checks alone did not show a clear game gain.
2. Investigate XGKICK/GIF synchronization granularity with dedicated correctness
   tests. Normal x64 microVU and its XGKICK-hack path have different contracts;
   copying whole-packet transfer blindly is unsafe. PATH arbitration, SIGNAL,
   writes to VU memory, interrupt timing, and exit-visible transfer credits need
   proper testing before reducing call frequency.
3. Measure why EE blocks average only five completed instructions and why some
   return zero. Source validation, dispatch frequency, unsupported operations and
   memory exits are distinct hypotheses. IOP interpreter/event work is another
   material cost and should not be hidden inside an “EE 99%” diagnosis.

Adding a few more VU arithmetic opcodes is unlikely to remove the main bottleneck:
fallback is a small sampled share. Nor does any single measured category explain
the entire deficit. Illustratively, eliminating all pipeline *and* XGKICK/GIF
sampled cost would imply only about 1.5x throughput if the sample were fully
representative and other costs stayed constant; this is an upper-bound estimate,
not a promised optimization result.

## Local artifacts

Ignored files under `build-arm64/` retain `investigate-intro.py`,
`analyze-diagnostic-sample.py`, `diagnostic-*.log/json`,
`diagnostic-arm.sample.txt`, and `diagnostic-sample-breakdown.txt`.
`root-cause-instrumentation.patch` preserves the temporary diagnostic changes.
No BIOS, game image, or save state is included in this document or commit.

The earlier checked-prefix/unchecked-tail experiment is saved separately in
`vu-tail-experiment.patch` and was not adopted. Its old/new/new/old medians over
frames 850–1300 were 24.581 / 24.3565 / 24.066 / 23.937 VPS, with no clear benefit.

After restoring the production sources, the ARM64 application build and deep code
signature verification passed, as did both CTest executables (160 unit tests).

## Follow-up: avoid premature EE branch event polling

The ARM64 driver was calling `_cpuEventTest_Shared()` on every branch test,
including before `cpuRegs.nextEventCycle`. The x86 `iBranchTest` checks the event
deadline first. This repeatedly invoked IOP synchronization and device checks
from short EE blocks. Applying the same deadline check only to native branch
exits measured 26.54 VPS; applying it to interpreted branch fallback during
native execution as well measured 28.38 and 27.89 VPS, with an intervening old
build at 24.83 VPS. CPU-thread time fell from 40.17 to 35.19–35.77 ms/frame.
These runs use the same frame selection and configuration described above.

The adopted change is the EE branch-polling policy, approximately 12–14% higher
throughput in this scene. Normal interpreter/boot polling, explicit CP0/MMIO
requests, and requested execution exits retain their forced behavior. Tests
cover deadline equality, overdue events, 32-bit/full-64-bit cycle boundaries,
and forced/interpreter polling. Runtime timing compatibility beyond the tested
scene still needs broader coverage. This does **not** achieve 60 FPS.

Other experiments were not adopted: 128-pair VU blocks measured 25.60 and 25.33
VPS against 24.75 VPS; adding compiled vector-cache transfer stubs measured
25.17 and 25.40 VPS, providing no additional gain. Their patch is retained as
`build-arm64/transfer-stub-experiment.patch`. Hot-block counting identified the
movie's repeated color-conversion microprogram, rather than an unexplained VU
spin loop. No game-specific instruction sequence was added to the backend.

Follow-up logs are `diagnostic-ee-{deadline-a,all-a,old,all-b}.log` under the
ignored build directory. Temporary metrics logging was removed from production.

The final production application and core test executable were rebuilt after
removing instrumentation. Deep code-signature verification and all 161 unit
tests passed. The additional production save-state smoke run was initially
blocked by the approval-review usage limit. After continuation, the existing
SCPS-15025 state loaded successfully, ran for 20 seconds without an early process
exit, and shut down with exit code 0. This is a smoke check, not a visual or
long-duration compatibility validation. Logs: `ee-deadline-state*.log`.

## Follow-up: EE lookup and rejected-entry caching

After the event fix, a separate 2,888-sample CPU capture classified 28.5% in the
shared VU pipeline, 19.8% in generated VU blocks, 13.0% in XGKICK/GIF, 15.5% in
EE dispatch/generated code, and 7.9% in IOP execution/events. These are sampled
shares, not a claim that VU pipeline work became more expensive per frame.
Artifacts: `diagnostic-sample-after-events.sample.txt` and
`post-event-sample-breakdown.json` in the ignored build directory.

A 1,024-entry front cache retains full virtual-PC tags and non-owning pointers to
the existing owning block map. Full mapping/source validation still occurs for
compiled blocks. Opcode-level rejection is reusable only when both the mapped
source pointer and first instruction word match; rejected branch/delay blocks
continue to validate both words. Cache reset/shutdown clears all lookup entries.
The table uses 24 KiB on ARM64.

With the event fix in both versions, the new/old/old/new sequence over frames
850–1100 measured **28.56 / 28.03 / 28.00 / 28.96 VPS**. New CPU-thread times were
34.93 and 34.45 ms/frame, versus 35.68 and 35.60 for the old version. This is a
modest approximately 2–3% improvement in this scene, not a route to 60 FPS by
itself. Logs: `diagnostic-rejected-{new,old}-{a,b}.log`.

Positive lookup caching alone measured 28.44 / 28.32 / 28.12 / 28.27 VPS, showing
little benefit. Separately, removing duplicate VU opcode-field stores passed
state-comparison tests but measured 27.91 / 28.31 / 27.78 / 28.16 VPS. It was not
adopted. That experiment remains in `vu-opcode-store-experiment.patch`.

New tests exercise lookup collisions, source mutation, virtual remapping,
reset/shutdown/restart, rejection becoming supported, and pointer validity across
owning-map growth. Temporary performance logging is removed from production.

Final production validation: ARM64 app/core-test build, deep code-signature
verification, and all 164 unit tests passed. The existing SCPS-15025 save state
loaded and ran for 20 seconds without early process exit, then shut down with
exit code 0 (`ee-lookup-state*.log`). No cross-platform or long-gameplay validation
was performed for this change.

## Root-cause follow-up: native XGKICK execution policy

The ARM64 provider was inheriting the interpreter's incremental GIF transfer
policy even when running native VU blocks. This differs from ordinary microVU:
`microVU_Analyze.inl::mVUanalyzeXGkick` schedules the kick with a one-cycle delay,
and `microVU_Compile.inl` emits the packet transfer after the following pair.
`microVU_Lower.inl::mVU_XGKICK_` then copies the whole packet. Only its separate
`XgKickHack` path uses incremental transfers. This is an execution-policy
mismatch, not a missing NEON arithmetic instruction.

The earlier counter run recorded 124 million transfer calls, 99.8% carrying
only 16 bytes. Each could cross from generated VU code into C++, publish/reload
cached vectors, and enter GIF parsing/arbitration. A temporary immediate-flush
probe measured 35.10 VPS against 29.33 VPS, but changed timing and was not adopted.

The adopted implementation shares the whole-packet copy helper with microVU and
makes the ARM64 dispatch boundary explicit: a pending kick permits one following
pair, publishes architectural state, then transfers its packet. This includes
that pair's lower store even if the pair stalls. Interpreter fallback, consecutive
kicks, E-bit termination and budget exits follow the same boundary. Completion
clears the busy bit and wakes VIF; forced packet completion does not charge VU
cycles per transferred qword. The interpreter and `XgKickHack` retain incremental
transfers. An already incremental packet finishes through the old path, including gaps
between tags; the existing enable word distinguishes its mode from a newly
scheduled native request.
Save-state layouts are unchanged. This follows microVU's normal timing policy;
it is not a claim of cycle-exact GIF timing or compatibility with every game.

With the existing setup and frames selected within 850–1100:

| Build | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| Previous implementation A | 29.02 | 34.38 | 3.36 |
| Previous implementation B | 29.14 | 34.22 | 3.36 |
| Final packet implementation A | 34.62 | 28.84 | 2.90 |
| Final packet implementation B | 34.57 | 28.85 | 2.96 |

This is approximately **19% higher throughput**, with CPU time falling from
34.30 to 28.84 ms/frame. An initial delayed-transfer prototype measured 34.53 VPS;
review then moved completion after the following pair to handle stalled stores
correctly. The final rows above use that corrected boundary and the explicit save-state
mode marker. Logs are `diagnostic-root-packet-{old,mode}-{a,b}.log` in the
ignored build directory.
This remains below 60 FPS. The measurements compare this fork before/after;
they are not a same-revision x64-versus-ARM64 benchmark.

A separate five-second capture at frame 900 collected 2,783 CPU samples after
the packet-policy change (before the final save-state mode marker): XGKICK/GIF accounted for 1.8%, generated VU pipeline management 27.7%,
generated VU blocks 23.9%, EE dispatch/generated execution 17.0%, and IOP
execution/events 9.6%. The prior post-event capture classified 13.0% in XGKICK/GIF.
These are sample shares from separate captures, not directly measured per-frame
costs. Sampling slows execution; its FPS is excluded from the table. Artifacts:
`diagnostic-sample-root-packet-final.sample.txt`, `analyze-root-packet.py`, and
`root-packet-sample-breakdown.json`.

The remaining large architectural gap is VU pipeline/flag management. ARM64
still constructs and retires interpreter FMAC queues and computes arithmetic
flags per pair. microVU carries compiler pipeline state between blocks and
analyzes flag liveness. Replacing those runtime operations requires materializing
correct state at budget exits and fallback; widening SIMD or adding threads
alone does not address that difference.

Final validation: production ARM64 app and core tests rebuilt without diagnostic
logging; all **176 tests** passed, including 12 new packet-policy tests covering
store visibility with/without stalls, fallback, pending budgets, consecutive
kicks, E-bit completion, counter/memory wrap, multiple tags, partial legacy
transfers, malformed size and interpreter/gamefix behavior. Deep application
signature verification passed. `Gif_Unit.cpp`, `VUops.cpp` and `x86/microVU.cpp`
also passed x86_64 syntax checks with the x64 build's `_M_X86=1` definition;
this is not an x64 runtime or full-link test. Production loaded the existing
SCPS-15025 save state, ran for 20 seconds without early exit, and shut down with
exit code 0. No visual or long-gameplay compatibility validation was performed.
Logs: `root-packet-production-{build,ctest,state,state-console}.log` and
`root-packet-x64-*.log` under the ignored build directory.


## Compile-time FMAC retirement schedules

The next change targets the remaining per-pair pipeline management. Like
microVU's dependency analysis, compilation tracks producer ages and stalls.
Starting after seven generic pairs, known timing allows emitting the exact
number of FMAC retirements. Incoming special pipelines, XGKICK, irregular FMAC
queues and counter wrap fail an entry guard and use the generic helper. At least
four scheduled pairs are required to amortize that guard. Queue slots remain
fully materialized so partial-budget execution remains interpreter-compatible.
Arithmetic flags are still calculated; only intermediate retirement writes that
cannot be observed between slots are combined, preserving every sticky bit.

Serial new/old/old/new runs, using the same frames 850–1100 and setup above:

| Build | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| Scheduled A | 35.84 | 27.87 | 2.99 |
| Previous A | 34.82 | 28.64 | 3.00 |
| Previous B | 34.58 | 28.81 | 3.01 |
| Scheduled B | 35.46 | 28.13 | 3.02 |

Mean throughput increased approximately **2.7%**, from 34.70 to 35.65 VPS.
These are modest same-fork gains, not a resolution of the gap to 60 FPS or a
Rosetta comparison. Logs: `diagnostic-scheduled-stalls-{new,old}-{a,b}.log`.

Later measurements became unusable for comparison with these rows: both a
further cycle-register-cache experiment and the unchanged baseline fell to
roughly 4–5 VPS, with GS time increasing from about 3 to 20 ms. The baseline's
startup MultiPause calibration also rose from about 65 to 208 ns. The cause of
this environment-wide change was not established. The cycle-cache experiment
was therefore excluded from the adopted change; none of those later timings
are used to claim a gain or regression here.

Two differential tests cover every budget prefix across seven instruction
patterns, plus pending special pipelines, irregular incoming timing and wrap.
They compare complete VU state and memory, not just final arithmetic results.
Broader gameplay compatibility still needs proper testing.

Final validation: the production ARM64 app and test binaries rebuilt without
benchmark logging; all 178 tests (20 common, 158 core) passed. Deep application
signature verification passed. The existing SCPS-15025 save state loaded, the
app remained running for 20 seconds and shut down with exit code 0. This does
not validate visual correctness, long gameplay or an x64 runtime build.
Logs: `scheduled-production-{build,ctest,state,state-console}.log` in the ignored
build directory.


## Instruction-level investigation after reboot

The unchanged `66a8d49da` comparison app measured 4.09 VPS before a host reboot
and 34.43 VPS afterwards, in the same frames 850–1100. The slow run spent about
212 CPU ms and 20 GS ms per frame; the reboot run returned to 29.02 and 2.88 ms.
AC power was active and `pmset -g therm` reported no recorded warning. A CPU
stack capture primarily showed executing emulator code rather than a wait.
These observations establish a host-state-dependent slowdown, but do not
identify its cause: frequency, core placement, App Nap and contention were not
measured directly. No global power/QoS changes were made. Logs:
`diagnostic-lowlevel-{baseline,reboot-baseline}.log`, `lowlevel-slow.sample.txt`.

On `71c36f4e3`, temporary compile-time markers associated generated ARM64 address
ranges with preparation, upper/lower execution, FMAC insertion, budget checks
and block entry/exit. A separate five-second capture at frame 900 collected
3,123 CPU-thread samples. No per-instruction runtime instrumentation was added.
The mapping and diagnostic logging are excluded from production.

| Sample location | Samples | CPU sample share |
| --- | ---: | ---: |
| Shared VU pipeline helper | 386 | 12.4% |
| In-block VU preparation | 157 | 5.0% |
| VU upper arithmetic, flags and stores | 343 | 11.0% |
| VU lower execution | 71 | 2.3% |
| VU FMAC queue insertion | 309 | 9.9% |
| VU cycle-budget checks | 270 | 8.6% |
| VU block entry/exit | 54 | 1.7% |
| EE dispatch and generated execution | 553 | 17.7% |
| IOP execution/events | 301 | 9.6% |

The remaining samples include VU dispatch/fallback, GIF and other CPU work.
This is instruction-pointer sampling, not a measurement of instruction latency,
cache misses, branch misses or hardware stall cycles. The budget region is only
a few instructions, including a cycle load immediately consumed by subtraction
and comparison. Queue insertion also reloads that cycle. The high sample share
motivates removing this memory dependency, but does not prove a cache miss.
The sampled run's FPS is excluded from throughput comparisons.
Artifacts in the ignored build directory: `lowlevel-code.map`,
`diagnostic-sample-lowlevel-reboot.sample.txt`, `analyze-lowlevel-reboot.py` and
`lowlevel-reboot-breakdown.json`.

The adopted change retains cycles in x26 across native pairs. It eliminates the
insertion/budget reloads and the scheduled path's cycle store/reload chain.
Generic preparation remains the authoritative boundary for incoming pipelines
and callbacks: publish a potentially dirty cycle before entry and reload after
return. Every block exit publishes it, including one-pair XGKICK limits and
partial budgets. Counter width/wrap and interpreter timing are unchanged.

This leaves substantial structural work. FMAC insertion still builds full
48-byte interpreter-compatible records per pair, and the generic prefix still
checks incoming queue state. microVU instead propagates pipeline state during
compilation. A larger redesign should first give block boundaries an explicit
pipeline-state contract and materialize state on fallback, callbacks and every
budget exit; merely suppressing queue writes would break those boundaries.
Arithmetic/flag work is another target, but its measured share alone cannot
explain the gap to 60 FPS. EE dispatch/source validation and the IOP interpreter
must also be accounted for. Hardware counter analysis would help distinguish
load dependencies from cache and branch effects before choosing more invasive
instruction-layout changes.

Serial new/old/old/new throughput runs after reboot, without stack sampling:

| Build | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| Cycle register A | 36.68 | 27.17 | 2.97 |
| Previous A | 35.78 | 27.90 | 2.98 |
| Previous B | 35.29 | 28.25 | 3.00 |
| Cycle register B | 35.91 | 27.74 | 3.00 |

The two-run means are **35.53 → 36.30 VPS, approximately 2.1%**, with CPU time
28.07 → 27.45 ms/frame. There is visible run-to-run variation; these measurements
support a small gain, not a claim that memory dependencies were the only
bottleneck. This remains well below 60 FPS. The baseline is `71c36f4e3`, unlike
the unchanged `66a8d49da` app used solely for the reboot comparison above.
Logs: `diagnostic-lowlevel-cycle-{new,old}-{a,b}.log`.

Production validation: all 178 existing tests passed after removing diagnostic
logging and address markers. These include full VU-state comparisons at every
budget prefix, incoming/stalled pipelines, cycle wrap, XGKICK boundaries and
pipeline callback ABI tests. The ARM64 app rebuilt, passed deep signature
verification, loaded the SCPS-15025 state, ran for 20 seconds without early exit,
and shut down with exit code 0. No visual, long-gameplay or x64 runtime test was
performed. Logs: `lowlevel-production-{build,ctest,state,state-console}.log`.

## Defer architectural queue construction within a complete suffix

The per-pair queue representation was the next design constraint to remove.
Rather than making each 48-byte record cheaper to write, the compiler now emits
a separate path for a contiguous scheduled suffix of at least eight pairs.
The entry guard establishes ordinary FMAC state with no pending FDIV, EFU,
IALU or XGKICK work. After the generic prefix, one exact budget check determines
whether the entire suffix can run without returning. Otherwise execution keeps
the existing per-pair path and its precise partial-budget exits.

On the complete path, q28..q31 retain MAC/status/clip snapshots for four logical
ring slots. Retired architectural STATUS/MAC values remain in GPRs. Static
producer fields and issue-cycle offsets are compiler state. At the end, the
compiler emits each overwritten slot's final representation once, including
inactive entries and cleared padding. Untouched slots retain their old bytes.
Queue indices/count, TPC, code, flags and cycles are published before leaving;
the common epilogue publishes VF/ACC. Arithmetic, lower memory accesses and VI
backup timing retain their original order. No callback, branch, E/D/T bit or
unsupported pair can occur in the deferred region. The general path shares the
same pair emitter and queue metadata encoder.

This removes repeated queue stores and dependent retirement loads, plus the
per-pair budget/TPC/code updates, rather than only optimizing their instruction
selection. It is the first deferred pipeline representation inside a block;
blocks still publish full state to one another. It does not introduce threads,
change guest timing or remove observable flags.

Serial new/old/old/new runs, same movie frames 850–1100, baseline `bd53fcfbd`:

| Build | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| Deferred suffix A | 40.93 | 24.36 | 2.92 |
| Previous A | 36.50 | 27.36 | 3.00 |
| Previous B | 36.73 | 27.18 | 3.00 |
| Deferred suffix B | 40.88 | 24.38 | 2.92 |

Mean throughput is **36.62 → 40.91 VPS, approximately 11.7% higher**; CPU time
falls from 27.27 to 24.37 ms/frame. These are unsampled measurements with MTVU
disabled. This is a larger gain than the earlier cycle-register change, but
still below 60 FPS and not a same-revision Rosetta comparison. The second path
increases generated code size, so broader workloads could have different gains.
Logs: `diagnostic-deferred-{new,old}-{a,b}.log` in the ignored build directory.

Two added differential tests cover all ring positions, zero-to-three overwritten
slots, longer wraparound, mixed flag-preserving operations, paired old-value
reads, VI backup, stalled chains, E-bit fallback and budgets on either side of
completion. A deterministic set of 128 mixed 64-pair programs covers partial
execution/resume, multiple blocks, arithmetic/conversions and data loads/stores.
Comparisons include complete VU state, all queue bytes, VU0/VIF state and data
memory; the tests have not relaxed checks for inactive pipeline entries.

Further major work remains at block boundaries: explicit incoming/outgoing
pipeline state, native control flow and linking could avoid repeatedly draining
or publishing the queue and restarting the generic prefix. This should build on
the materialization contract above, with separate validation for branch delays,
callbacks, save states and budget exits, rather than simply omitting state writes.

Final validation: the production ARM64 app and test binaries rebuilt without
measurement logging; all **180 tests** (20 common, 160 core) passed. Deep signature
verification passed. The existing SCPS-15025 state loaded, ran for 20 seconds
without early exit, and shut down with exit code 0. No visual, long-gameplay or
x64 runtime validation was performed. Logs:
`deferred-production-{build,ctest,state,state-console}.log`.


## Refresh scheduling readiness after incoming special work retires

The deferred path retained one eligibility decision from block entry. Pending
IALU work disabled scheduling for the entire block even when it retired during
the generic prefix. Increasing block length then extended generic execution
instead of amortizing its management overhead.

Temporary counters on the 64-pair candidate recorded 15,826,944 deferred-boundary
visits, 11,870,208 entry-guard passes and the same number of budget passes.
All 3,956,736 guard failures reported pending IALU; FDIV/EFU/XGKICK counters were
zero. A separate 128-pair run passed the entry guard on only 4,143,104 of
8,286,208 visits, again with no further budget failures. These are whole-boot
counts, not just the timed movie interval. The initial hypothesis that larger
blocks failed the suffix budget check was therefore rejected. Diagnostic counter
runs are excluded from throughput comparisons.

Eligibility now has two stages. Incoming FMAC validity, cycle-wrap protection
and absence of XGKICK establish a permanent safety condition. Special-queue
readiness can become true after generic preparation drains FDIV/EFU/IALU.
Generated code checks this at the first scheduled pair and the suffix boundary,
not at every pair. Invalid incoming FMAC state and possible XGKICK callbacks
remain permanently ineligible. Native supported pairs cannot issue new special
work or incur IALU branch stalls, so readiness stays valid once established.

The selected block cap is 128 pairs, with code-buffer margin and cycle-wrap
protection scaled accordingly. VI backup countdown updates are accumulated until
a VI write or suffix exit. Source validation still covers every cached pair;
full architectural state is published at exits. There is no cross-block linking,
threading change or relaxed emulated timing.

Exploratory unsampled runs, frames 850–1100, MTVU disabled:

| Candidate | VPS |
| --- | ---: |
| Previous 32-pair implementation (`a1595a4b9`) | 40.89 |
| 64 pairs | 42.35 |
| 64 pairs, accumulated backup countdown | 43.12 |
| 128 pairs, accumulated backup countdown, stale guard | 40.29 |
| 64 pairs, accumulated backup countdown, refreshed readiness | 46.63 |
| 128 pairs, accumulated backup countdown, refreshed readiness | 48.81 |

The 128-pair regression disappears after fixing readiness. This supports the
management-path explanation; no hardware cache-miss or stall-counter claim is
made. Larger generated blocks may still perform differently on other workloads.

Serial new/old/old/new confirmation against `a1595a4b9`, same interval/settings:

| Build | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| Refreshed readiness, 128 pairs A | 48.81 | 20.42 | 3.05 |
| Previous A | 41.30 | 24.13 | 2.90 |
| Previous B | 41.38 | 24.10 | 2.91 |
| Refreshed readiness, 128 pairs B | 48.17 | 20.66 | 3.08 |

Mean throughput is **41.34 → 48.49 VPS, approximately 17.3% higher**. CPU time
falls from 24.12 to 20.54 ms/frame. The first new run is the same 128-pair run
listed above, followed by the two baseline repeats and the final new run.
Logs: `diagnostic-readiness128-a.log`,
`diagnostic-readiness-final-{old-a,old-b,new-b}.log`. Sampling and diagnostic
counters were disabled; the temporary metrics logger was identical in both
apps. This remains below 60 VPS and is not a same-revision Rosetta comparison.

Differential tests now cover 128-pair blocks, changes beyond the old 32-pair
source boundary, micro-memory and cycle wrap, full queue bytes, byte-saturated
VI backup countdown, mixed instructions and partial execution/resume. A new test
varies pending IALU/FDIV/EFU completion around the scheduling boundary, including
Q consumers, invalid incoming FMAC timing and near-wrap state. These establish
state equivalence; the separate counters and timed runs establish the performance
path diagnosis.

Production validation: rebuilt the ARM64 app and test binaries without metrics
logging; all **182 tests** (20 common, 162 core) passed. Deep signature verification
passed. The SCPS-15025 state loaded SPU2/GS, ran for 20 seconds without early exit,
and shut down with exit code 0. Both baseline and new development apps report
missing optional `patches.zip`; these runs do not validate bundled game patches.
No visual, long-gameplay or x64 runtime validation was performed. Logs:
`readiness-production-{build,ctest,state,state-console}.log`.


## Compare larger linear-block limits

The scheduling-readiness fix makes larger blocks worth measuring again.
`MaxInstructions` limits guest instruction pairs per compiled block;
`MaxBlockBytes` is a conservative free-space threshold before code generation,
not a separate throughput setting or the amount allocated to every block.
Raising the latter alone can cause earlier cache resets without reducing guest
execution overhead. The existing 2 KiB-per-pair margin is retained.

The 128/256/512 candidates differ only in the instruction limit. They use the
same temporary metrics logger, movie frames 850–1100 and MTVU-disabled settings.
Builds, tests and measurements run serially. Larger-block differential coverage
includes 255/256/257 and 511/512/513 pairs, full queue bytes, partial budgets,
cycle wrap and source changes at pairs 191 and 447. These are interpreter-state
comparisons rather than assertions that a larger limit is always faster.

Measured order: 128 A, 256 A, 512 A, 512 B, 256 B, 128 B.

| Pair limit | VPS A | VPS B | Mean VPS |
| --- | ---: | ---: | ---: |
| 128 | 48.92 | 48.15 | 48.53 |
| 256 | 50.42 | 50.72 | 50.57 |
| 512 | 49.94 | 50.04 | 49.99 |

Select **256 pairs**: mean throughput improves by **4.2%**
relative to 128 pairs. The 512-pair version was slightly slower than 256 in both
passes. This supports a conservative 256-pair cap for this workload, not a claim
of a universal optimum. Larger limits can change register selection, generated
code footprint, source-validation work and whole-suffix budget eligibility;
these runs do not isolate their individual contributions. Compilation cost is
also relevant: the present age analysis visits earlier producers quadratically.
Increasing the cap indefinitely is therefore not a substitute for reducing
cross-block state publication and generic-prefix work.

Logs: `diagnostic-limits{128,256,512}-{a,b}.log` and corresponding JSON files in
the ignored build directory. All 182 tests passed on both candidate limits.

Final 256-pair production validation: metrics logging removed, app rebuilt,
all **182 tests** passed, deep signature verification passed, and the existing
SCPS-15025 state loaded SPU2/GS and ran for 20 seconds with normal shutdown.
Logs: `limits-production-{build,ctest,state,state-console}.log`. The existing
missing optional patches.zip warning remains; visual correctness, long gameplay
and x64 execution were not validated in this experiment.


## Boundary investigation after the 256-pair change

A fresh 5-second CPU-thread sample contained 3,253 samples. Call-tree attribution
estimated VU dispatcher work at 4.55% and interpreter fallback at 5.90%, with
substantial EE/IOP work remaining. These are statistical software samples, not
hardware stall or cache-miss measurements. The instrumented sample run is not a
throughput result. Artifacts: `diagnostic-sample-boundaries.{sample.txt,log}` and
`boundary-sample-breakdown.json` in the ignored build directory.

Separate whole-boot counters recorded 12,961,762 native block calls, of which
9,819,570 (75.8%) contained fewer than 15 pairs. Only 3,338,799 entered with all
FMAC/FDIV/EFU/IALU/XGKICK queues empty. Thus the 256-pair maximum does not describe
most boundary visits. This is a call count, not the fraction of CPU time or the
movie-only interval. See `diagnostic-boundary-stats.log`.

A prototype executed supported branch delay pairs natively, limited validation
to the one pair that could execute, skipped unused schedule guards at forced
single-pair entries, and added terminal unconditional B. Differential tests
passed, but throughput did not improve convincingly: individual delay-only
variants measured 51.05, 50.57 and 51.03 VPS; the complete B/delay prototype
measured 50.26 and 49.62 VPS, versus interspersed baseline runs at 50.86 and
50.55 VPS. The prototype was removed rather than adding control-flow complexity
without a measured benefit. The tests remain as coverage for future branch work.
Logs: `diagnostic-boundary-{delay-a,prefix-a,single-a,branch-a,branch-b,old-a,old-b}.log`.

The x64 reference in `microVU_Branch.inl::normBranchCompile` searches for a target
block with matching pipeline state and jumps directly to its native entry.
Simply making an isolated delay pair native retains the ARM64 dispatch, register
publication/reload and generic preparation boundaries. Full native control flow
will need an explicit compatible entry/exit state contract; the measurements do
not justify treating isolated native branch support as equivalent to linking.

The selected smaller change removes unused host SIMD saves at existing native
boundaries. Previously every block using any cached vector register saved and
restored all eight d8..d15 registers. It now saves the used count rounded up to
an even number for paired stores and 16-byte stack alignment. For one or two
cached registers, the SIMD save area shrinks from 64 to 16 bytes, removing three
STP/LDP pairs per invocation. Guest VF/ACC publication and pipeline timing are
unchanged. An executable ABI test checks all eight host registers with zero to
eight cached vectors, partial exits, complete execution and E-bit fallback.

Serial save-reduction / baseline / save-reduction confirmation measured
**50.65 / 50.55 / 50.56 VPS** (CPU **19.70 / 19.72 / 19.69 ms/frame**).
There is **no demonstrated material throughput gain** in this movie. The change
is retained for its smaller, directly verifiable boundary save/restore work and
unchanged state/ABI behavior, not as a claimed FPS improvement. Logs:
`diagnostic-boundary-save-{a,old,b}.log`. All temporary counters and metrics
logging were removed. The next substantial optimization needs compatible
pipeline/register state across native edges and precise publication at budget,
callback, interpreter and save-state boundaries. EE and IOP costs also remain;
M5 single-core capability alone cannot attribute the remaining slowdown to VU1.

Final production validation: ARM64 app and test binaries rebuilt without
instrumentation; **187 tests** (20 common, 167 core) passed. Deep signature
verification passed. The SCPS-15025 save state loaded SPU2/GS, ran for 20 seconds
and shut down with exit code 0. The existing optional patches.zip warning remains.
No visual, long-gameplay or x64 runtime validation was performed. Logs:
`boundary-production-{build,ctest,state,state-console}.log`.


## Connect static unconditional branches within a bounded native trace

The first connection stage follows unconditional B through a supported delay
pair into its destination while decoding a single native trace. Destination code
is laid out next in the host instruction stream. All regions share the vector
cache assignment and producer-age analysis, so internal edges need no VF/ACC
publication/reload, host ABI exit/re-entry, or new generic scheduling prefix.
The deferred queue representation can also span these edges when its existing
readiness and whole-suffix budget conditions hold.

Each instruction records its architectural successor and whether it completes a
branch delay. Both ordinary and deferred emission update branch/TPC state, so
partial budgets can return after B with a pending delay or after the delay with
the destination selected. A resumed pending delay uses the existing interpreter
path. Conditional/indirect branches, nested delay branches, E/D/T control and
unsupported instructions retain their previous handling. Restored chained-delay
state conservatively avoids traces containing B. An already-visited PC ends the
trace; native loop back-edge linking and independent compiled-target patching
are not implemented yet. This bounds compilation and avoids introducing mutable
cross-cache code pointers in the first stage.

Source checking now records contiguous ranges in execution order and validates
all of them before entry. It covers destination changes and branch retargeting,
not only the bytes adjacent to the original PC. The same 256-pair code-size and
cycle-wrap limits apply. New tests compare every budget from 1 through 192 on a
stalled, non-contiguous graph, including target edits, retargeting, nested branch
fallback and micro-memory wrapping. All architectural queue bytes, VF/VI/ACC,
VU0/VIF state and memory continue to be compared against the interpreter.


Serial baseline/new/new/baseline runs on frames 850–1100, MTVU disabled:

| Build | VPS | CPU ms/frame |
| --- | ---: | ---: |
| Baseline A (`10091df92`) | 50.88 | 19.61 |
| Connected trace A | 50.98 | 19.55 |
| Connected trace B | 50.88 | 19.59 |
| Baseline B | 50.18 | 19.87 |

This small difference is **not evidence of a movie speedup**. A separate
diagnostic run recorded 12,961,762 native entries and **zero** entries into traces
containing B by the last snapshot at 196,608 Execute calls. The newly supported
connection is exercised by differential tests, not by this movie workload.
Logs: `diagnostic-link-{old-a,new-a,new-b,old-b,stats}.log`. Counter runs are not
included in the throughput table.

A second diagnostic identified the lower opcode fields at ordinary fallback
entries (excluding already-pending delay slots):

| Lower opcode | Meaning | Count at final snapshot |
| --- | --- | ---: |
| 0x04 | ILW | 9,819,350 |
| 0x2d | IBGTZ | 3,142,192 |
| 0x40 | Extended lower opcode group | 3,536,506 |

These are cumulative boot/intro call counts, not CPU-time percentages or counts
restricted to frames 850–1100. ILW and IBGTZ account for about **78.6%** of these
fallback entries. They identify the next useful extension: integer-load pipeline
latency and VI backup semantics must be integrated with conditional branch
decisions and delay-slot retirement before connecting those edges. The 0x40
bucket has not yet been broken down into individual instructions. This corrects
the earlier assumption that unconditional B would exercise the movie hot path.
Artifact: `diagnostic-link-opcodes.log`; temporary diagnostics were removed.

Final validation: the production ARM64 app rebuilt without diagnostic counters
or metrics logging. **190 tests** (20 common, 170 core) passed, including the new
pending-XGKICK test that ensures a connected branch cannot run its delay store
before the packet transfer. Deep signature verification passed. The SCPS-15025
state loaded SPU2/GS, ran for 20 seconds and shut down normally. The existing
optional patches.zip warning remains. No visual, long-gameplay or x64 runtime
validation was performed. Logs: `link-production-{build,ctest,state,state-console}.log`.


## Native ILW and IBGTZ with integer-pipeline timing

The fallback counts identified ILW and IBGTZ as active work, unlike unconditional
B in this intro. ILW now performs its halfword load natively and issues the
reference four-cycle IALU record, preserving queue padding and inactive entries.
It does not use arithmetic BackupVI. IBGTZ uses a new shared preparation entry
which applies matching VI load stalls after upper FMAC stalls and before pipeline
retirement, then selects the signed current/backup VI value and records a taken
branch with one delay pair remaining.

ILW invalidates transient scheduling readiness; its own pair and four following
pairs use generic retirement. The compiler rechecks readiness when a known
schedule resumes. Thus pending integer work is never silently omitted from the
deferred representation. A conditional terminal pair is kept outside that
representation: the preceding body can still defer queue construction and flow
into branch preparation with VF/ACC cached. Exact budget exhaustion exits before
the branch. Conditional successors and pending delay slots still use dispatch;
this change does not claim conditional edge linking.

Tests cover all ILW component masks, VI0, base/destination aliasing, halfword
preservation, signed/wrapped addresses, inactive queue padding, VI backups,
matching and unrelated integer hazards, combined FMAC/IALU waits, cycle wrap,
load bursts and resumption of static scheduling. Every budget across deferred
bodies and taken/not-taken conditional tails is compared against the interpreter.
The private-ABI callback test also exercises the seventh (integer branch) entry.

Exploratory measurements: baseline 50.46 VPS; native ILW/IBGTZ with a separate
conditional entry 51.96 VPS; including the conditional tail in its preceding
native trace 52.09 VPS. These measure the same frames 850–1100 with MTVU disabled.
They show a modest benefit, not evidence that video decoding itself is the root
cause. Logs: `diagnostic-int-{old-a,new-a,tail-a}.log`.

Serial final/new baseline comparison, in new/old/old/new order:

| Run | VPS | CPU ms/frame |
| --- | ---: | ---: |
| int-tail-a | 52.09 | 19.12 |
| int-old-b | 51.02 | 19.53 |
| int-old-c | 50.63 | 19.69 |
| int-tail-b | 52.73 | 18.90 |

Mean throughput improves **50.83 → 52.41 VPS (3.1%)** against
`665b161c5`. This remains below 60 VPS. The runs were unsampled and serial, with
identical temporary metrics logging in both apps; logs are
`diagnostic-int-{tail-a,old-b,old-c,tail-b}.log`. The change retains shared FMAC
retirement and adds native integer pipeline/branch handling; it does not establish
that the remaining frame-time cost belongs to a video decoder. Direct conditional
successor linking, other extended lower operations and EE/IOP work remain outside
this change.

Production validation: metrics logging removed, ARM64 app and core tests rebuilt,
all **195 tests** (20 common, 175 core) passed, and deep signature verification
passed. The SCPS-15025 state loaded SPU2/GS, ran for 20 seconds and shut down with
exit code 0. The existing optional patches.zip warning remains. No visual,
long-gameplay or x64 runtime validation was performed. Logs:
`int-production-{build,ctest,state,state-console}.log`.

## Taken IBGTZ connections and budget-limited source validation

Taken IBGTZ edges now continue through a supported delay pair into the target
within the same bounded trace and VF/ACC assignment. Not-taken paths and exact
budget exhaustion publish complete state through the common exit. Repeated PCs,
nested branches and unsupported instructions still end the trace. This is not
native loop back-edge linking or independently cached block linking.

An integer branch can introduce an unknown IALU wait, so retirement analysis
forgets uncertain producer ages at that pair. Later pairs recover known timing.
Deferred queue construction now applies to each sufficiently long scheduled
region, with queues materialized before conditional exits. The deferred emitter
restores scheduling readiness before continuing, since it borrows that register
for architectural flags.

The initial implementation applied deferred emission only before the first
conditional branch and regressed to **39.00 VPS**. Extending it to all eligible
regions recovered **50.44 VPS**, still below the current baseline. A separate
instrumented diagnostic then showed approximately 470,573 calls each at byte PCs
4128 and 4136 per million native calls. These entries contained 247 and 246 pairs
respectively; the former starts with ILW immediately before IBGTZ, whose target
is byte PC 2168. Short calls were validating almost the entire connected loop
body even when their remaining budget could not reach it. These counts describe
the diagnostic run, not CPU-time shares or proof of a video-decoder bottleneck.

Validation now checks at most the remaining cycle budget's number of pairs,
across all relevant source ranges. Every pair advances at least one cycle, so
omitted bytes cannot execute in that call. Subsequent calls validate those bytes
before they can execute; changing a reachable branch or target still invalidates
the cached trace. A pending packet transfer uses a one-pair bound consistently
for validation and execution.

Final serial comparisons against `66574d2f9`, MTVU disabled, frames 850–1100:

| Run | VPS | CPU ms/frame |
| --- | ---: | ---: |
| taken-old-a | 52.43 | 19.00 |
| taken-prefix-a | 52.38 | 19.01 |
| taken-old-b | 51.78 | 19.25 |
| taken-prefix-b | 52.58 | 18.96 |

Means are **52.11 → 52.48 VPS (+0.7%)**. This is effectively unchanged within
run-to-run variation; no substantial speedup is claimed. The connection is a
correctness-tested foundation, and native loop continuation remains unresolved.
The frequency of short budget-limited entries also means eliminating a branch
fallback alone does not eliminate host dispatch. Logs are
`diagnostic-taken-{old-a,old-b,prefix-a,prefix-b}.log`; the slower exploratory and
counter runs are `diagnostic-taken-{new-a,regions-a,diag}.log`.

New differential tests cover multiple conditional regions, taken/not-taken
paths, long incoming integer waits, every budget across those paths, source
edits and retargeting, nested delay fallback, wrapped delay addresses and the
256-pair limit. A separate test changes initially unreachable source bytes and
then increases the budget. A packet-transfer test verifies that a connected
conditional delay store cannot run before the pending transfer observes memory.

Production validation: diagnostic logging/counters removed, ARM64 app and tests
rebuilt, **199 tests** (20 common, 179 core) passed. Deep signature verification
passed. The SCPS-15025 state loaded SPU2/GS, ran for 20 seconds and exited normally.
The existing optional patches.zip warning remains. No visual, long-gameplay or
x64 runtime validation was performed. Logs:
`taken-production-{build,ctest,state,state-console}.log`.

## Continue natively across an incoming XGKICK packet boundary

Follow-up counters resolve the short-entry ambiguity above. In the sampled
million-call windows, byte PC 4128 had approximately 470,573 calls: every call
was marked `PacketXgkickPending()`, had an artificially limited one-cycle
budget, executed one cycle and returned at byte PC 4136. None exhausted the
original caller budget. Calls at 4136 had roughly three million cycles available
on average and no pending packet. This is a transfer-publication boundary, not
frequent exhaustion of EE catch-up budgets. `BaseVUmicroCPU::ExecuteBlock()`
already uses a minimum of 16 cycles for ordinary catch-up. Diagnostic log:
`diagnostic-reentry-diag.log`; its instrumented throughput is not a benchmark.

Native entry now receives a pending-packet flag. After the first pair, including
its lower store and branch retirement, a shared private-ABI helper publishes
VF/ACC, completes the existing packet transfer, reloads the cache and returns to
the same generated frame. The cycle budget is checked after this boundary, so
exact-budget exits still complete the due transfer. The compiler rechecks its
full scheduling guard after the callback instead of permanently disabling
scheduled regions because a packet was pending on entry. Architectural queues
remain materialized at the callback. Source validation uses the actual caller
budget, since the same entry may now execute the connected destination.

This preserves the established delayed whole-packet policy and GIF arbitration;
it changes neither the caller's cycle allowance nor the interpreter/gamefix
transfer behavior. XGKICK issuance itself still uses interpreter fallback, and
native loop back-edge linking remains future work. In particular this removes
one redundant entry/exit at a transfer boundary, not the transfer itself.

Tests retain existing store-order, stalls, conditional delay, nested-kick,
wrap, gamefix and state-restoration coverage. The private-ABI test now exercises
packet completion with zero, one and eight cached vectors, callback mutations
and host upper-vector clobbers. A new test compares continuous execution with
execution split at the packet boundary for every budget from 1 through 260,
including subsequent stores, scheduled FMAC regions and a conditional edge;
full VU states, data memory and the copied GIF packet must agree.

Serial measurements against `b13631068`, same frames 850–1100, MTVU disabled:

| Run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| reentry-new-a | 53.10 | 18.78 | 3.10 |
| reentry-old-a | 52.41 | 19.01 | 3.10 |
| reentry-old-b | 42.76 | 22.47 | 4.09 |
| reentry-new-b | 51.29 | 19.31 | 3.22 |
| reentry-old-c | 51.74 | 19.24 | 3.16 |
| reentry-new-c | 52.35 | 19.01 | 3.17 |

Pairs a and c suggest a small **1.3% and 1.2%** gain respectively. Run old-b has
substantial variation in both CPU and GS timing; its cause was not established,
so it is retained here but not treated as evidence of a large speedup. This is
not a dramatic throughput improvement or proof of video decoding as the main
cost. All benchmark runs used identical temporary metrics logging and no
reentry counters. Logs: `diagnostic-reentry-{new-a,old-a,old-b,new-b,old-c,new-c}.log`.

Production validation: counters and metrics logging removed, ARM64 app rebuilt,
**200 tests** (20 common, 180 core) passed, and deep signature verification passed.
The SCPS-15025 state loaded SPU2/GS, ran for 20 seconds and exited with code 0.
The existing optional patches.zip warning remains. No visual, long-gameplay or
x64 runtime validation was performed. Logs:
`reentry-production-{build,ctest,state,state-console}.log`.

## Native XGKICK issuance and same-trace loop back edges

Ordinary native XGKICK no longer returns to the opcode interpreter. Its emitter
flushes an old request through the shared cache-aware helper, reads the low VI
address afterwards, initializes the architectural packet fields and sets the
VU0 busy bit. Its upper operation and FMAC issue retain reference ordering.
The following pair completes the packet after its stores and branch retirement;
a consecutive kick flushes the previous request and leaves the new one delayed.
Both issuance and delayed completion break deferred scheduling, and timing
analysis recovers known producer ages before resuming scheduled regions.
XgKickHack retains interpreter issuance. The transfer policy is part of the
code-cache options and the shared pipeline's compiled configuration.

The expanded reference comparison exposed an existing credit-only discrepancy:
with XgKickHack enabled, a restored native packet marker stayed marked as native
until a later transfer-helper call. The reference downgrades it on the first
pipeline tick. The shared preparation stub now performs that downgrade even
when it takes the credit-only path.

The decoder also records back edges which return to the same trace entry with
no unresolved branch delay. Such edges keep the VF/ACC assignment and host frame,
check the original budget, publish the cycle, rerun the pipeline guard and branch
to the generic incoming-state preparation. A kick in the final pair carries its
pending flag into the next iteration. Other internal repeated PCs still exit;
there are no pointers between independently invalidated compiled blocks.
Completing a whole iteration implies that its source bytes were all within the
initial validation budget. Supported VU stores and GIF callbacks do not mutate
microcode, so those bytes need not be compared again each iteration.

The new differential test uses interpreted opcode steps under the existing
native packet policy as its reference. It compares complete VU0/VU1 state, data
memory and copied GIF bytes across every tested budget: consecutive kicks,
branch-delay kicks, conditional delayed pairs, the 256-pair trace boundary,
conditional loop exits and a kick carried across the back edge. It also covers
old incremental/native requests and both XgKickHack settings. The shared ABI test
now exercises flushing an old incremental request as well as packet completion,
including zero/one/eight cached vectors and callback clobbers.

An exploratory native-issuance-only run reached **53.20 VPS**, versus **53.00 VPS**
for the current baseline: effectively unchanged. Same-trace loop continuation
is measured separately below. These are throughput measurements, not evidence
that video decoding or integer arithmetic is the dominant cost.

Serial final comparison against `186de5b75`, frames 850–1100, MTVU disabled:

| Run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| kick-old-a | 53.00 | 18.80 | 3.11 |
| kick-loop-a | 54.62 | 18.25 | 3.13 |
| kick-old-b | 53.01 | 18.81 | 3.10 |
| kick-loop-b | 54.19 | 18.40 | 3.13 |

Mean throughput is **53.00 → 54.40 VPS (+2.6%)**. This is a modest improvement,
still below 60 VPS in this interval. Both apps used the same temporary metrics
logging; no profiling counters, concurrent builds or tests ran during measurement.
Logs: `diagnostic-kick-{new-a,old-a,loop-a,old-b,loop-b}.log`.

Production validation: temporary metrics logging removed, ARM64 app rebuilt,
**201 tests** (20 common, 181 core) passed, and deep signature verification passed.
The SCPS-15025 state loaded SPU2/GS, ran for 20 seconds and exited with code 0.
The existing optional patches.zip warning remains. No visual, long-gameplay or
x64 runtime validation was performed. Logs:
`kick-production-{build,ctest,state,state-console}.log`.

## NEON reduction of MAC flag lanes

A fresh five-second sample at intro frame 900 still contains generated-code
execution, GIF packet transfers and repeated `intExecuteBlock` frames from
`R3000AInterpreter.cpp`. The JIT frames are not individually symbolized, so this
sample does not establish a per-instruction bottleneck or a precise VU1/IOP time
split. Its instrumented VPS value is not used in performance comparisons.
Artifact: `diagnostic-sample-next.sample.txt`.

The MAC emitter previously extracted each active NEON lane into a general
register and combined its four flag bits there. It now applies the architectural
lane weight (8, 4, 2, 1, or zero for an inactive lane) to the classification masks,
then uses ADDV and one vector-to-integer transfer. Each lane contributes distinct
bits within zero, sign, underflow and overflow groups, so addition introduces no
carry between flags. A fully enabled mask removes six emitted instructions from
this part of MAC generation, while adding a 16-byte literal. Input/output clamps,
FPCR-dependent zero/underflow behavior, reserved MAC bits and STATUS reduction
retain their existing semantics. This is a local code-generation change, not
flag-liveness elimination or a new pipeline representation.

An additional experiment retained current MAC/STATUS in a register across
callback-free deferred regions to remove stores and reloads. It measured
**53.04 VPS**, versus **54.22 VPS** in the subsequent baseline run, and was
removed. No claim is made about the hardware cause of that regression. The final
candidate retains only the NEON lane reduction.

Focused differential tests cover every lane mask, all tested rounding/FZ and
input/output-clamping combinations, signed zero, minimum normals, overflow,
NaNs, preserved upper MAC bits, and both issued and retired records. Additional
long-region tests interleave arithmetic with non-arithmetic pairs and compare
complete flags and queues across partial/full budget exits.

Serial final comparison against `738e3db34`, frames 850–1100, MTVU disabled:

| Run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| flags-new-a | 54.88 | 18.13 | 3.29 |
| flags-old-a | 54.22 | 18.39 | 3.19 |
| flags-old-b | 53.95 | 18.48 | 3.16 |
| flags-new-b | 55.04 | 18.10 | 3.15 |

Mean throughput is **54.08 → 54.96 VPS (+1.6%)**. This is a small improvement,
not a resolution of the remaining 60-VPS gap. All four runs had identical
metrics logging, no sampling, and no concurrent builds or tests. Logs:
`diagnostic-flags-{new-a,old-a,old-b,new-b}.log`;
the discarded register-caching run is `diagnostic-flags-cache-a.log`.

Production validation: temporary metrics logging removed, ARM64 app rebuilt,
**203 tests** (20 common, 183 core) passed, and deep signature verification passed.
The SCPS-15025 state loaded SPU2/GS, ran for 20 seconds and exited with code 0.
The existing optional patches.zip warning remains. No visual, long-gameplay or
x64 runtime validation was performed. Logs:
`flags-production-{build,ctest,state,state-console}.log`.

## IOP instruction fetch overhead

`VMManager::UpdateCPUImplementations` still selects `psxInt` on ARM64, while x64
can select `psxRec`. The earlier sample contains repeated IOP interpreter frames,
but does not establish IOP's fraction of CPU time. Inspection found that every
IOP opcode fetch, and J's import-table delay-slot probe, called the general
`iopMemRead32` device/memory dispatcher even for ordinary RAM.

`iopMemFetch32` now inlines the aligned main-RAM case through the current RLUT.
It only handles physical addresses below 8 MiB, retains page mirrors and virtual
aliases, and reloads both the mapping and instruction on every access. It does
not cache decoded instructions or require write invalidation. Unmapped entries,
unaligned addresses, ROM and hardware regions use the existing read function;
in particular, a nonzero hardware/SIF RLUT entry never bypasses its handler.
Cycle accounting, branch delay slots, event polling and debug hooks are unchanged.

Three focused tests cover RAM page boundaries and aliases, instruction edits,
remapping/unmapping, ROM/expansion fallback and the hardware scratchpad path even
when its RLUT points at different data. They supplement the existing EE/VU tests;
PS1 execution, other host architectures and long gameplay remain unvalidated.

Serial comparison against `fd2fdf4f1`, frames 850–1100, MTVU disabled, in execution
order:

| Run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| iop-old-a | 54.60 | 18.23 | 3.20 |
| iop-new-a | 56.70 | 17.58 | 3.18 |
| iop-old-b | 54.65 | 18.23 | 3.19 |
| iop-new-b | 54.92 | 18.08 | 3.29 |
| iop-new-c | 55.51 | 17.92 | 3.24 |
| iop-old-c | 54.11 | 18.41 | 3.16 |

Mean throughput is **54.45 → 55.71 VPS (+2.3%)**. All three paired comparisons
improved, but the size varied substantially (about 0.5–3.8%); this is not a
precise universal speedup or a solution to the remaining 60-VPS gap. Both apps
used the same temporary metrics logging, without sampling or concurrent builds
or tests. Logs: `diagnostic-iop-{old-a,new-a,old-b,new-b,new-c,old-c}.log`.

Production validation: temporary metrics logging removed, ARM64 app rebuilt,
**206 tests** (20 common, 186 core) passed, and deep signature verification passed.
The SCPS-15025 state loaded SPU2/GS, ran for 20 seconds and exited with code 0.
The existing optional patches.zip warning remains. No visual, long-gameplay or
x64 runtime validation was performed. Logs:
`iop-production-{build,ctest,state,state-console}.log`.

## IOP leaf dispatch and NOP

The IOP interpreter previously dispatched grouped instructions through a basic
opcode handler followed by a second indirect dispatch; COP2 basic operations
used an additional grouping level. `psxExecuteOpcode` now resolves these groups
inline and calls the original leaf function. It retains the existing tables
and opcode implementations rather than introducing another decoded cache or
copying instruction semantics. Exact NOP skips the empty SLL-to-r0 handler.
The execution driver still performs its debugger/logging hooks, PC increment and
cycle increment before this dispatch, including NOPs and branch delay slots.

A differential routing test replaces only leaf handlers with unique probes,
retaining the original grouping functions as the reference. It compares 131,072
combinations covering every primary, function and register selector, including
unsupported encodings. Another test verifies that both legacy and new NOP
execution preserve the full register state, including a nonzero r0 backing value.
These checks do not replace PS1 or long-gameplay validation, which remains needed.

Serial comparison against `e08ac048c`, frames 850–1100, MTVU disabled, in execution
order:

| Run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| dispatch-old-a | 56.36 | 17.69 | 3.24 |
| dispatch-new-a | 57.18 | 17.44 | 3.09 |
| dispatch-new-b | 57.00 | 17.50 | 3.11 |
| dispatch-old-b | 56.15 | 17.74 | 3.23 |

Mean throughput is **56.26 → 57.09 VPS (+1.5%)**. Both paired comparisons improved
by a similar amount, but this is a small, scene-specific result from two runs per
variant. It does not establish how much time all IOP interpretation consumes,
or isolate NOP elimination from the grouped-dispatch change. Both apps used the
same temporary metrics logging, without sampling or concurrent builds/tests.
Logs: `diagnostic-dispatch-{old-a,new-a,new-b,old-b}.log`.

Production validation: temporary metrics logging removed, ARM64 app rebuilt,
**208 tests** (20 common, 188 core) passed, and deep signature verification passed.
The SCPS-15025 state loaded SPU2/GS, ran for 20 seconds and exited with code 0.
The existing optional patches.zip warning remains. No visual, long-gameplay or
x64 runtime validation was performed. Logs:
`dispatch-production-{build,ctest,state,state-console}.log`.

## IOP branch event polling

Temporary instruction and branch counters identified a much more concentrated
workload than general integer arithmetic. Fourteen 10-million-instruction chunks
reported between the logged intro frames 872 and 1095 contained 140,001,909
instructions: exact NOP accounted for 45.84%, and J for 45.10%. This frequency
does not by itself prove that those jumps are all safe idle loops, nor measure
the time cost of individual opcodes. The first chunk can include work before
frame 872. Diagnostic artifact: `diagnostic-iop-counts.log`.

Of 64,203,310 taken-branch event scans in those chunks, 64,010,108 (**99.70%**)
occurred before `iopNextEventCycle`. These diagnostics added per-instruction
counters and timers, so their VPS is not used as a performance baseline. The
recorded ExecuteBlock elapsed time also includes instrumentation and scheduling;
it is not an exact CPU-time attribution.

Unlike the x64 recompiler's deadline check in `iPsxBranchTest`, the IOP
interpreter unconditionally called `iopEventTest` at every taken branch. It now
checks the same signed 64-bit cycle difference before scanning scheduled events.
It additionally retains the interpreter's immediate response to an already
pending, enabled hardware interrupt, using the existing CP0 Status/ICTRL/
ISTAT/IMASK condition after the delay slot. This matters because MTC0 and RFE can
change interrupt eligibility before the scheduled deadline. The EE-side
unconditional scan and device/counter scheduling APIs are unchanged. No guest
instructions or cycles are skipped, and no idle-loop fast-forwarding is added.

Six integration tests execute actual J/delay-slot programs through `psxInt`.
They cover future/due deadlines, crossing both the 32-bit and full 64-bit cycle
boundaries, immediate pending interrupts against the existing event handler,
MTC0 and RFE enabling/disabling interrupts in the delay slot, future device and
counter scheduling, and the PS1 fractional EE/IOP cycle conversion. More games
and real PS1 execution still need proper testing.

Serial comparison against `e57f028bc`, frames 850–1100, MTVU disabled, in execution
order:

| Run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| poll-old-a | 56.97 | 17.50 | 3.17 |
| poll-new-a | 58.32 | 17.11 | 3.17 |
| poll-new-b | 57.73 | 17.28 | 3.17 |
| poll-old-b | 54.68 | 18.13 | 3.34 |
| poll-old-c | 57.04 | 17.47 | 3.16 |
| poll-new-c | 57.66 | 17.29 | 3.17 |

Mean throughput is **56.23 → 57.91 VPS (+3.0%)**. All three paired comparisons
improved, but old-b was slower than the other baselines, including increased GS
time. Paired gains range from about 1.1% to 5.6%; the average should not be treated
as a precise universal speedup. Both apps had the same temporary metrics logging,
with no instruction counters, sampling or concurrent builds/tests. No run was
excluded. Logs: `diagnostic-poll-{old-a,new-a,new-b,old-b,old-c,new-c}.log`.

Production validation: temporary diagnostics removed, ARM64 app rebuilt,
**214 tests** (20 common, 194 core) passed, and deep signature verification passed.
The SCPS-15025 state loaded SPU2/GS during the 20-second smoke run; the log records
normal device shutdown and host-memory release. The process exit code was not
captured after the tool session ended. The existing optional patches.zip warning
remains. No visual, long-gameplay or x64 runtime validation was performed. Logs:
`poll-production-{build,ctest,state,state-console}.log`.

## Outdoor gameplay state: VU1 instruction coverage

The user supplied an outdoor gameplay state (`saved_state/AGENTS.p2s`) for
SCPS-15025. Its screenshot is an interactive scene, unlike the earlier opening
movie. The state, screenshot and extracted memory are not included in commits.
`build-arm64/investigate-scene.py` loads that state with the same private runtime
configuration (1x resolution, MTVU disabled), then compares frames 120–420 with
no controller input. Every run starts from the same state.

Before sampling began, metrics showed about 25 VPS, roughly 40 ms CPU time per
frame, and only 1–2 ms GS/GPU time. The five-second sample contains extensive
`Arm64VU1Recompiler::Execute` → `vu1Exec` fallback, including FMAC/IALU stall and
queue processing. This is evidence of CPU-side overhead, not a rendering
resolution bottleneck. The sample's own perturbed VPS is excluded from all
comparisons. Artifact: `scene-sample-baseline.sample.txt`.

A separate temporary counter run recorded 355 million interpreter steps and
about 180 million native entries over the state run. Native entries advanced
only about 4.44 VU cycles on average. The unsupported lower-opcode histogram
included about 57.6 million LQI, 53.5 million SQI, 53.5 million IBNE and 26.6 million
IBEQ occurrences. Unsupported upper instructions also included CLIP and
OPMULA/OPMSUB. Counts include fallback delay slots and warmup; upper/lower counts
can overlap on the same pair and are not percentages of elapsed CPU time.
No timing result from this instrumented run is used as a benchmark.
Artifact: `scene-vu-counts.log`.

The implementation extends the existing VU1 emitter instead of increasing block
size or changing emulated cycle budgets:

- LQI/SQI/LQD/SQD transfer selected vector components and update the low 16 bits
  of the address VI. Memory wrapping, VF0/VI0 behavior, full encoded-register
  guards and VI backup creation match the interpreter. A conflicting upper
  destination still suppresses the entire lower operation.
- IBEQ/IBNE reuse the existing taken-edge trace, pipeline preparation, delay-slot
  handling and shared exit machinery. Each operand independently selects its
  current or backed-up VI value. Matching IALU hazards are retired before the
  comparison. Nested branches retain fallback, and a back edge to the trace
  entry can reuse the native frame and cached vectors.

Four differential tests cover every transfer lane mask, address and VI wrapping,
zero and extended register encodings, VI backup state, upper/lower conflicts,
deferred regions, both equality operands, pending IALU work, partial budgets,
cycle wrap, nested branches, native loops and source-code edits. Comparisons
include both VUs' complete register/queue state and VU1 data memory; native-code
allocation is checked to avoid passing through silent fallback alone.

Serial unsampled comparison against `7c26e3402`:

| State run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| old-a | 25.38 | 39.25 | 1.24 |
| new-a (transfers only) | 29.02 | 34.47 | 1.22 |
| combined-a | 31.58 | 31.64 | 1.19 |
| old-b | 25.82 | 38.70 | 1.21 |
| combined-b | 31.66 | 31.55 | 1.19 |

The final two-run means are **25.60 → 31.62 VPS (+23.5%)**. The intermediate
transfer-only result is shown separately and excluded from those means. Both
final comparisons improved consistently. All runs used identical temporary
metrics logging, without instruction counters, sampling or concurrent builds
or tests. Logs: `scene-{old-a,new-a,combined-a,old-b,combined-b}.log`.

An opening-movie regression check on frames 850–1100 measured **57.85 → 57.62
VPS (-0.4%)**, a small difference from a single pair rather than a precise
regression bound. Logs: `diagnostic-scene-{old,new}.log`.

The supplied state still runs well below 60 VPS. CLIP, outer-product operations,
DIV, flag-test instructions and other unsupported instructions continue to
fragment execution. Their recorded frequency identifies further candidates;
it does not establish the remaining speedup available from each one. The
installed `/Applications/PCSX2.app` was not replaced during this investigation.

Production validation: temporary metrics and VU counters removed, ARM64 app
rebuilt, **218 tests** (20 common, 198 core) passed, and deep signature verification
passed. The supplied outdoor state loaded SPU2/GS, ran for 20 seconds and exited
with code 0. The existing optional patches.zip warning remains. No visual,
long-gameplay or x64 runtime validation was performed. Logs:
`scene-production-{build,ctest,state,state-console}.log`.


## Outdoor-state CPU attribution and native CLIP

A fresh five-second CPU-thread sample at `4fcd1d6d5` was partitioned by stack
ancestry and the runtime EE/VU1 JIT address ranges. Self samples sum to the
thread's 2,752 samples without negative or unassigned counts:

| CPU-thread category | Samples | Share |
| --- | ---: | ---: |
| VU1 native code and pipeline | 1,023 | 37.2% |
| VU1 interpreter fallback | 552 | 20.1% |
| VU1 dispatcher/compiler | 205 | 7.4% |
| VU1 GIF transfer | 12 | 0.4% |
| IOP | 137 | 5.0% |
| All remaining EE/VU0/VM work | 823 | 29.9% |

VU1 therefore remains the largest subsystem at approximately **65%**. Temporary
emitter cursor logging mapped the shared pipeline to VU1 cache offsets
`0x000–0x714` (scan `0x234`, retirement `0x2a8`, backup `0x5a8`). This code contains
532 self samples, **19.3% of the CPU thread**: 21 in entry stubs, 166 in hazard
scanning, 268 in retirement/transfer handling and 77 in backup/finish handling.
These are sampling estimates, not exact per-instruction timings. The sampling
run is excluded from speed comparisons. Artifacts:
`scene-sample-after-transfers.{log,sample.txt}`, its `-partition.json`, and
`scene-map-pipeline-console.log`.

The change extends the existing native trace with CLIP and FCAND/FCEQ/FCOR,
reducing fallback breaks without introducing a second pipeline model. As in
microVU, clipping uses integer comparisons; the ARM64 implementation specifically
matches the interpreter's denormal-W threshold, non-finite bit patterns and
24-bit history. NEON compares XYZ against both signs of W and reduces weighted
bits. Lower flag tests observe the retired CLIP instance. Reads of architectural
flags terminate deferred regions; CLIP retirement already excludes the ordinary
scheduled-retirement path. Remaining pipeline handling is deliberately retained.

Three differential tests cover signed zero, denormals, infinities/NaNs, ignored
CLIP masks, flag-test immediates, upper VI preservation, backup state, pending
CLIP producers, aliases including VF0, same-pair lower vector writes, cycle wrap,
and partial budgets through ordinary and deferred regions. Comparisons include
full registers, queues and VU1 memory. Native allocation checks reject silent
fallback-only success for the new instructions.

Serial unsampled runs of the supplied state (frames 120–420, identical settings
and temporary metrics, no concurrent builds/tests) measured:

| Run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| old-a | 30.82 | 32.37 | 1.22 |
| new-a | 33.41 | 29.89 | 1.22 |
| old-b | 30.61 | 32.70 | 1.27 |
| new-b | 33.57 | 29.74 | 1.20 |

Two-run means are **30.72 → 33.49 VPS (+9.0%)**, with CPU time
**32.54 → 29.81 ms/frame**. Artifacts: `scene-clip-{old,new}-{a,b}.log`.
The previous session's baseline was faster than this session's; only these
interleaved runs are used for the improvement calculation.

A separate post-change sample contains 2,763 CPU samples: native VU1/pipeline
1,064, fallback 463, VU1 dispatcher/compiler 252 and VU1 GIF transfer 12.
VU1 remains approximately **65%**, with fallback's share approximately **17%**.
This does not imply that native execution slowed down: the sampling shares use
different total work rates and are not absolute timing comparisons. Unimplemented
outer products and DIV, plus the shared pipeline and trace boundaries, remain
candidates. Additional native coverage alone has not removed the management
cost, and the state remains far below 60 VPS. Artifacts:
`scene-sample-after-clip.{log,sample.txt}` and its `-partition.json`.

Opening-movie regression check (frames 850–1100): **57.62 → 57.53 VPS (-0.2%)**,
a single pair rather than a precise regression bound. Logs:
`diagnostic-clip-{old,new}.log`. Temporary metrics and emitter-offset logging
were removed before the production build. The installed app was not replaced.


## VU1 interpreter fallback attribution and native DIV

Temporary instrumentation counted every VU1 instruction pair reaching
`Arm64VU1Recompiler::Step()`, keyed by its encoded upper/lower words, over one
run of the supplied state. It recorded **273,259,728 native block entries against
298,100,106 interpreter steps**: the interpreter still executed more pairs than
the generated code. The distribution is extremely narrow, as expected for a hot
microprogram loop; the twenty most frequent pairs cover 279.6M of the 298.1M
steps. Decoding those against the interpreter's own opcode tables gives:

| Pairs | Lower | Upper | Status |
| ---: | --- | --- | --- |
| 59.9M | DIV | CLIP / NOP | Unsupported lower |
| 56.6M | LQ, IBEQ | OPMULA, OPMSUB | Unsupported upper |
| 38.9M | ILWR | NOP | Unsupported lower |
| 31.3M | FMAND | MULw, NOP | Unsupported lower |
| 17.8M | ISW | NOP | Unsupported lower |
| 11.4M | IBLTZ, IBGEZ | CLIP, NOP | Unsupported lower |
| 9.3M | WAITQ | ADDq | Unsupported lower |
| 15.7M | RSQRT | ADDq, NOP | Unsupported lower |

An unsupported pair does not merely execute itself through the interpreter: it
ends the trace being built, so the recompiler cannot span it. DIV was therefore
the single largest item, and its pipe was already modelled — the generic
retirement body in `VU1Pipeline.cpp` has always retired the FDIV slot, but
nothing in generated code ever filled it.

Implementing DIV required three things beyond the arithmetic. The interpreter
stalls a new FDIV issue on an outstanding entry and then retires that entry
before overwriting the single slot, so generated code has to do both in order.
A stall taken inside a pair has to be published to `VURegs.cycle`, because the
block only writes back its cached cycle register for scheduled pairs. And a
deferred region skips shared preparation entirely, so nothing would retire the
FDIV slot while its entry is outstanding; pairs within the pipe's latency now
stay on the generic path, matching what ILW already does for the IALU pipe. Each
of these was found by a differential test rather than by reading the reference.

Three differential tests cover division by zero with every sign and zero
combination, denormal and non-finite operands, both operand lane selectors,
aliased Fs/Ft, cycle wrap, back-to-back issue inside the latency window, and
budget prefixes across the retirement boundary. The three missing unary integer
branches were added alongside, reusing the existing branch path.

Serial unsampled interleaved runs of the supplied state (frames 120-420,
identical settings and temporary metrics, no concurrent builds or tests):

| Run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| base-a | 30.97 | 32.23 | 1.17 |
| new-a | 33.07 | 30.19 | 1.16 |
| base-b | 31.07 | 32.11 | 1.20 |
| new-b | 32.78 | 30.43 | 1.20 |

Two-run means are **31.02 -> 32.93 VPS (+6.1%)**, CPU time 32.17 -> 30.31
ms/frame. An earlier 28.81 VPS figure from the instrumented build is excluded:
the per-step counter itself cost roughly 7%.

204 tests passed. The state still runs far below 60 VPS, and the table above is
the remaining work: the outer products are now the largest single item, followed
by ILWR, FMAND/FSAND, ISW and the rest of the FDIV/EFU families. No visual,
long-gameplay or x64 runtime validation was performed.


## VU1 ILWR, the status/MAC/CLIP flag-test family, and ISW

Continuing the same attribution table: ILWR (38.9M fallback pairs), the FMAND
family (31.3M), and ISW (17.8M) were the next three largest sources after DIV.
All three reuse existing infrastructure rather than adding any:

ILWR is ILW without the immediate offset (VI[Is] is already the quadword
index) and shares ILW's IALU pipe timing and lane-priority selection exactly,
so it needed no scheduler changes.

FCSET, FCGET, FSEQ, FSSET, FSAND, FSOR, FMEQ, FMAND and FMOR all read a
retired flag instance (status, MAC or CLIP) and combine it with either an
encoded immediate or VI[Is], or write a staging flag field for later FMAC-pipe
retirement (FCSET, FSSET). The existing FCAND/FCEQ/FCOR code already
established this shape and already forces deferred regions to end before an
instruction reads STATUS/MAC/CLIP, so these needed no new scheduling logic
either — just the same pattern nine more times.

ISW differs from ILW/ILWR in that it writes each masked lane independently
(X/Y/Z/W are separate destination addresses, not a single priority-selected
lane), so it does not skip when It == 0. It reduces to broadcasting VI[It]
into all four lanes and reusing the existing StoreMasked helper.

Two hundred and seven tests pass, including new differential coverage for
each of these instructions (edge-value sweeps, mask/register/offset sweeps,
and budget sweeps through the FMAC-pipe retirement boundary for FCSET/FSSET).

Serial unsampled interleaved runs of the supplied state (frames 120-420):

| Run | VPS | CPU ms/frame | GS ms/frame |
| --- | ---: | ---: | ---: |
| base-a | 33.01 | 30.25 | 1.16 |
| new-a | 34.89 | 28.58 | 1.17 |
| base-b | 33.31 | 29.96 | 1.18 |
| new-b | 35.03 | 28.49 | 1.18 |

Two-run means are **33.16 -> 34.96 VPS (+5.4%)**, CPU time 30.10 -> 28.54
ms/frame.

OPMULA/OPMSUB (56.6M pairs, the single largest remaining item) were attempted
in this session and reverted. Classifying OPMSUB as supported in DecodeUpper
reproduces a VF-register corruption in the existing
`SpecialFloatsAndChangedFloatingPointOptions` differential test **even with a
completely empty emission body** (`return;` with no LoadVector, no arithmetic,
no store at all) — so the bug is in the block scheduler's response to this
opcode's classification and real interpreter-provided register metadata
(VFwxyzw fixed at 0xE rather than derived from the encoded mask; VIread
carries REG_ACC_FLAG, which the structurally-identical existing MSUB opcode
also carries, so that alone does not explain it), not in anything this
session emitted for it. Substituting a pre-existing opcode into the same slot
in the same 64-pair trace does not reproduce the corruption. Forcing the pair
to end a deferred region did not resolve it either. This needs a proper
root-cause before OPMULA/OPMSUB can be added; it may be a pre-existing
scheduler gap that simply had no supported opcode long/unusual enough to
reach before now, and may be resolved by the static-scheduling pipeline
redesign already outlined for this backend, rather than by patching the
per-op path further.

The state still runs far below 60 VPS. Remaining large items by measured
frequency: OPMULA/OPMSUB (blocked, see above), RSQRT (~15.7M), WAITQ (~9.3M),
IBLTZ/IBGEZ (~11.4M, now implemented alongside DIV in the previous commit).
No visual, long-gameplay or x64 runtime validation was performed.

## OPMULA/OPMSUB: native implementation and a known cross-block hazard gap

Re-investigated the OPMULA/OPMSUB corruption from the previous session with a
smaller, targeted reproduction instead of the full game trace. Two findings
narrowed it considerably:

- The corruption does not depend on OPMSUB's own emitted arithmetic at all:
  a deliberately-empty body and a deliberately-wrong body (reusing the
  existing MSUB emission path without the outer-product lane shuffle) produce
  byte-identical corruption in the same unrelated registers. Disabling
  deferred-region compilation entirely (`EmitDeferredRegion`) also does not
  change it. This rules out `StoreMAC`/`StoreVector`, the deferred-region
  flag-ring (`EmitFmacMetadata`), and the shared retirement code in
  `VU1Pipeline.cpp` (which only ever writes STATUS/MAC/CLIP, never VF data).
- Bisecting the `SpecialFloatsAndChangedFloatingPointOptions` failure by
  budget shows the actual mechanism: OPMSUB's own destination register reads
  back a transiently wrong value for several pairs after it retires (still
  within its 4-cycle FMAC latency), then self-corrects once a later,
  unrelated write reaches the same register. A pair in the *next* compiled
  block, which reads that register with `dependency == -1` (cross-block,
  resolved by the generic runtime hazard scan in `VU1Pipeline.cpp` rather
  than the compile-time `ins.dependency` window), lands inside that
  transient-wrong window, latches the bad value into its own destination,
  and nothing rewrites it again before the budget ends. OPMSUB simply
  happens to be the first opcode that reaches this exact topology (last
  pair of a block, consumed again a few pairs into the next one); the gap
  is in the cross-block hazard path, not in OPMULA/OPMSUB's own metadata.

With that separated out, the actual arithmetic was implemented and checked
against the scalar interpreter (`_vuOPMULA`/`_vuOPMSUB` in
`pcsx2/VUops.cpp`) term by term: `Fd.x = ACC.x - Fs.y*Ft.z`, `Fd.y = ACC.y -
Fs.z*Ft.x`, `Fd.z = ACC.z - Fs.x*Ft.y` (OPMULA writes the plain product to
ACC instead). NEON has no single instruction for this permutation, so both
Fs and Ft are rotated into place with three `Ins` lane copies each before an
ordinary `Fmul`/`Fmls`. The destination mask is hardcoded to 0xE — real
hardware never reads a mask field for these two opcodes, unlike every other
FMAC instruction — so `StoreMAC` gained an explicit mask-override parameter
rather than reading `(code >> 21) & 15`.

206 of 207 tests pass. `SpecialFloatsAndChangedFloatingPointOptions` still
fails on the cross-block hazard gap described above; it was not fixed this
session; see the notes above for where to continue.

Serial interleaved runs of the supplied state (frames 120-420):

| Run | VPS | ms/frame |
| --- | ---: | ---: |
| old-a | 35.01 | 28.56 |
| new-a | 36.71 | 27.24 |
| old-b | 35.37 | 28.27 |
| new-b | 36.55 | 27.36 |

Two-run means are **35.19 -> 36.63 VPS (+4.1%)**, CPU time 28.42 -> 27.30
ms/frame. A 25-second unmeasured run of the same state exited cleanly on
SIGTERM with no crash. No visual, long-gameplay or x64 runtime validation
was performed.


## RSQRT and SQRT

The next two largest measured fallback items after OPMULA/OPMSUB. Both share
the FDIV pipe machinery DIV already established (the stall/retire/overwrite
handling and the deferred-region `fdiv_ready` exclusion), so the shared
pending-stall check and pipe-writeback code were pulled out of the DIV
handler into `EmitFDIVStall`/`EmitFDIVFinish` and reused by all three.

SQRT (`_vuSQRT`) is `q = sqrt(fabs(ft))`, with the I status flag set when
`ft < 0`; RSQRT (`_vuRSQRT`) is `q = fs / sqrt(fabs(ft))`, with a deeper
zero-handling case than DIV's own division-by-zero path when `ft == 0`
(distinguishing `fs == 0` from `fs != 0` for the I/D flag combination and the
signed-zero/signed-max-float result). Both use plain ARM64 `Fsqrt`/`Fdiv`
after the usual `ClampInput` denormal/overflow handling.

One correctness detail worth flagging: the `ft < 0` test cannot use the
`lt`/`ge` condition codes after `Fcmp`, because AArch64 defines those to
treat an unordered (NaN) comparison as if it were "less than", the opposite
of the interpreter's plain C `<` (always false for NaN). `pl`/`mi` (which
key off the N flag alone) give the correct false-for-NaN result and are used
here instead.

Two new differential tests sweep zero, signed zero, denormals, normals and
non-finite bit patterns on both operands (matching the existing DIV test's
coverage), through the same pipe-retirement budget boundaries. 208 of 209
tests pass; `SpecialFloatsAndChangedFloatingPointOptions` remains the one
known failure from the previous section, unrelated to this change.

Serial interleaved runs of the supplied state (frames 120-420) were far
noisier than previous sessions on this machine (up to 9% swing between
otherwise-identical runs of the *same* binary):

| Run | VPS | ms/frame |
| --- | ---: | ---: |
| old-a | 34.44 | 29.04 |
| new-a | 33.70 | 29.68 |
| new-b | 36.98 | 27.04 |
| old-b | 36.36 | 27.50 |
| new-c | 36.51 | 27.39 |

Old mean (a, b): 35.40 VPS. New mean (a, b, c): 35.73 VPS — roughly **+1%**,
within this run's noise floor. RSQRT/SQRT's combined ~15.7M fallback pairs
are considerably fewer than OPMULA/OPMSUB's 56.6M, so a smaller and noisier
measured effect than the previous session's is expected rather than a sign
of a problem. A 25-second unmeasured run exited cleanly on SIGTERM with no
crash. No visual, long-gameplay or x64 runtime validation was performed.
