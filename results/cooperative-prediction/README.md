# Cooperative predictor implementation evidence

Baseline: `953a11e`. Validation was performed on the working-tree snapshot of
`cooperative-mpc-prediction` before publication. See Git history for the published commit. See [source/evidence hashes](provenance.json) and
[the interface, activation conditions and limitations](../../docs/COOPERATIVE_PREDICTION.md).
No physical CPU/board energy, battery life, general Linux advantage or
hard-real-time guarantee is established.

## What changed and what was verified

- RLS, expert blending and the existing temporal LMS are owned by one
  replaceable predictor. MPC uses const candidate queries; it still owns the
  decisions, constraints, frequency timing and existing work/power objective.
- A real delayed-learning synthetic ramp, without injected LMS weights or
  quality flags, changes an MPC decision from 2.4 to 1.8 GHz. The raw guard
  subsequently forces 1.2 GHz. This proves functional influence, not savings.
- Tests reject stale, unsupported-action/power/horizon, invalid and nonfinite
  predictions; reject negative relaxation; verify repeated query purity,
  duplicate-observation handling and caller-owned replacement backends. An
  independent numerical thermal-ODE oracle checks candidate-power response.
- Nominal independent/joint historical controller reports match byte for byte.
  Twenty-six original forecast case JSON outputs match the pre-change portable
  run. Existing rule/forecast behavior is retained in nominal mode.
- A separate frozen-source audit compiles the original controller and all three
  original learning sources from SHA-256-pinned `953a11e` bodies. Only controller
  state-field paths and public symbols are mechanically adapted to the new
  header. All 30 nominal finite-job cases match the original on each tested
  platform, excluding instrumentation counters/CPU timing.
- The Linux build, complete model/closed-loop suite, file-ABI e2e, native
  deployment/recovery fixtures and ABI 2 tests passed. The source peer's 56
  compiled ABI values match. This change does not alter shared records.
- A real VM systemd manager passed cooperative mode with special-character
  executable paths and optional Lertick discovery: unlimited ready service,
  independent waiting for an absent peer, stopped-worker watchdog recovery,
  sensor-loss recovery, and stop/disable/restoration on uninstall. It used only
  uniquely named services and temporary hardware files, not real CPU controls.
  [Build/recovery log](linux-validation-and-systemd.log).

This is not a new active-peer scheduling benchmark. In particular, a waiting
peer test is not proof of a currently connected live scheduler. The existing
ABI 2 integration evidence remains separate.

## Identical finite work: full results, including regressions

The [60 raw rows](finite-jobs.jsonl) contain 30 A/B pairs: five workload
patterns, three fixed seeds and independent/joint MPC. Each run receives the
same 2,880 FIFO jobs as its paired controller. Arrivals do not depend on chosen
frequency. Unfinished service remains queued; after arrivals stop, every job
must drain. The independent service ledger must match plant `work_done` within
a fixed numerical tolerance. Completed work is not the historical `served`
percentage.

Patterns are capacity-normalized sustained work, two-second bursts,
asymmetric cluster work, deterministic varying work, and a sustained queue
with an ambient ramp from 25 toward 35 C. These are development fixtures, not
unseen real applications or independent hardware samples. The plant still
uses assumed thermal/power parameters.

All 60 runs completed all jobs and had zero remaining backlog and simulated
throttle steps. Startup-inclusive sensor peak was at most **82.23 C**. In
28/30 pairs the control and forecast outcomes were identical. The remaining
two pairs were the ambient-ramp independent-MPC cases:

| Seed | Makespan, nominal → cooperative | Job p99, nominal → cooperative | Modeled board energy change | Point MAE change |
| --- | --- | --- | --- | --- |
| 7 | 979.53 → 979.55 s | 609.65 → 609.67 s | -0.00412% | +0.5873% |
| 99 | 981.83 → 981.90 s | 611.96 → 612.02 s | -0.00205% | -0.4288% |

Each used one corrected control query. Query counts are evaluations, not a
count of distinct actions or a measure of saved energy. The differences are
tiny, at the 10 ms model's time resolution; no statistical significance or
hardware gain is claimed. Pinned per-CPU FIFO overload explains the large
job p99 and is not an application responsiveness estimate.

**The strict nonregression gate fails on two pairs.** All adverse results stay
in [the paired summary](finite-jobs-summary.json) and [gate log](strict-gate.log).
There is no basis to promote cooperative prediction to the default. The
summary test also injects a lower-energy/lower-MAE case with worse completion
and latency, and verifies that it is reported as a regression rather than a win.

The gate's tiny-difference comparison is an audit, not a significance test.
Reported CPU time includes only controller computation in the simulator
harness. A/B run order is fixed, not randomized; timing differences cannot be
attributed causally to the prediction mode. The predictor occupies 14,368 bytes
per cluster on the tested ABI, including existing learners and the additional
action-validation queue. Whole-daemon RSS, worst-case planning deadlines and
physical sysfs overhead require separate measurement.

The [ARM64 Linux rows](finite-jobs-linux.jsonl) and
[Linux summary](finite-jobs-summary-linux.json) have the same A/B conclusions.
An additional absolute cross-platform equality audit **failed** on four rows
(the nominal/cooperative versions of asymmetric joint seeds 42 and 7).
These differences also occur in the frozen original; each platform's refactored
nominal matches its original, as shown by the
[macOS](nominal-reference-macos.json) and [Linux](nominal-reference-linux.json)
reference audits. [All platform differences](cross-platform-comparison.json)
are retained. Their numerical/compiler root cause has not been established;
bitwise cross-platform reproducibility is not claimed.

## Native daemon smoke runs

Both prediction modes completed a separate 180-second Linux daemon run on the
same step-load trace family, scored after 120 seconds. These are explicitly
smoke runs, shorter than the historical confirmation protocol. They established
thermal-model readiness, valid mode telemetry and complete restoration of all
30 knobs, with no journal remaining, no bad writes and no simulated throttling.
Both reached a simulator sensor peak of at most 48.1 C.

| Mode | Rows with a ready thermal model | Corrected control queries | Point MAE / its reference MAE | Daemon user + system CPU time |
| --- | ---: | ---: | --- | ---: |
| Cooperative | 896 | 0 | 0.554107 / 0.531929 C | 0.091581 s |
| Nominal | 896 | 0 | 0.559519 / 0.534160 C | 0.088484 s |

The point forecast is worse than its reference in this short step-load window.
No supported control correction activates here. These runs do not establish
prediction improvement, lower energy or real-time bounds. Separate live runs
have timing differences; their error/CPU-time differences are not a causal
comparison of modes. The tiny modeled-energy difference is not a saving claim.
[Runtime summaries and provenance](runtime/summary.json),
[raw archive](runtime/raw-archive.json) and
[Linux sanitizer/runtime log](linux-sanitizers-and-runtime.log) preserve results.

## Retained development failures

1. An early reset-on-action implementation discarded forecast weights whenever
   frequency/power changed. The first finite-job run showed worse point MAE
   and no correction queries. That design was rejected. The implemented form
   resets only control-validation state and preserves the original learners.
   The first run is retained under ignored `results/local/`; it was not an
   independent confirmation set and has no frozen source snapshot.
2. The first Linux deployment suite crashed in the checkpoint regression:
   a newly added private-backend check accessed state before validating the
   declared policy count. [The failed log](linux-initial-failure.log) remains
   intact. Count/fingerprint validation now precedes state access; the existing
   invalid-count fixture and ASan/UBSan roundtrips pass after the fix.
3. A duplicate-timestamp no-op guard initially exposed an invalid-observation
   test that reused the same timestamp. The fixture now sends a new timestamp
   for the invalid sample and separately asserts duplicate observations are
   byte-for-byte no-ops. This did not change controller thresholds.
4. A final backend-rejection test initially failed to compile because a fixture
   condition was placed in the observation callback instead of prediction.
   The shell then ran an older binary; that output was not accepted as evidence.
   The fixture was corrected, subsequent validation used `set -e`, and the full
   suite plus sanitizers passed. [Compiler failure](development-fixture-compile-failure.log).
5. The initial local VM status attempt was sandbox-blocked. The explicitly
   scoped VM access succeeded; this was not a controller or model failure.

No controller parameters were swept to optimize these finite-job outcomes.
The old evidence and failed attempts were not overwritten to claim a pass.

## Reproduction

```sh
make test
build/test_cooperative_loop --full > /tmp/cooperative-finite.jsonl
python3 tests/summarize_cooperative.py --input /tmp/cooperative-finite.jsonl \
  --output /tmp/cooperative-summary.json
# Deliberately reports failure for the recorded strict performance comparison:
python3 tests/summarize_cooperative.py --input /tmp/cooperative-finite.jsonl \
  --require-nonregression
# Requires the original baseline Git object; an offline pinned-source option
# is available for source-only Linux mirrors. Use a fresh output directory.
python3 tests/verify_nominal_reference.py --output /tmp/lerwatt-original-audit

# Linux, normal user; controls are temporary fixtures:
make all e2e test-deployment
python3 tests/check_integration_abi.py --peer ../lertick
# Explicitly authorized disposable systemd VM only:
sudo python3 tests/test_service_systemd.py --allow-systemd-test \
  --prediction cooperative --special-prefix --with-lertick --daemon build/lerwattd
```

Longer native daemon/file-ABI runs are labeled smoke unless the original
20-minute/5-minute-warmup confirmation protocol is met. Any new native smoke
results are activation/timing/restoration evidence, not physical power or
throughput evidence. Source/archive identities and runtime outcomes are
recorded with the final validation artifacts.
