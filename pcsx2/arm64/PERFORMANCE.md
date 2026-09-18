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
