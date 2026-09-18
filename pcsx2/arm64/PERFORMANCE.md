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
