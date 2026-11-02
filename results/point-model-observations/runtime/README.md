# Real daemon and simulated file-ABI confirmation

The predictor and simulator binaries are frozen at `fe05326`. Collection uses
six sequential 1200-second cases, each scored after 300 seconds, with no other
VM workload experiment running. Both predictions are issued together and
compared with the same raw sensor five seconds later. The first observation
at or after the target is used only when no more than 250 ms late. End-of-run
predictions without a future label are unscored. Every observation, including
fallback, remains eligible; delayed labels are excluded from both forecasters.

The predeclared gate requires at least 3% reductions in equal-profile mean
MAE and RMSE, with no case regressing more than 2% in either metric. These are
six simulated input profiles, not six independent real boards. Overlapping
five-second labels do not provide per-point confidence intervals.

These input families already appear in the 32-profile module study. Their
selection was fixed before runtime collection; this checks integration and
live timing rather than introducing six unseen workload families. Module and
runtime percentage reductions must be reported separately.

## Collection corrections and retained failures

- The first collection attempt was interrupted because its restoration
  snapshot omitted the actual `pm_qos_resume_latency_us` filename. All 22
  captured knobs and all eight fresh-fixture PM QoS defaults were checked
  afterward. This attempt is retained and is not counted as a complete case.
- The next collector captured all 30 knobs. Its first three cases completed.
  The random-load case then failed a byte-for-byte restoration assertion:
  three frequency maxima had the original integer value but no trailing
  newline. All 30 integers were independently verified unchanged, the journal
  was gone, and both processes exited zero. The original failed `run.json`
  stays unchanged. It is not counted as a completed confirmation.
- `ec5a696` changes the test snapshot to validated integer values, matching the
  sysfs ABI while rejecting malformed content or changed numeric settings.
  A short helper smoke test passed before the full random, step and sparse
  cases were repeated. No predictor, simulator binary or acceptance threshold
  changed after viewing results.
- The corrected helper stops the simulator immediately after daemon shutdown
  and restoration, before offline JSON scoring. Earlier cases left it running
  during scoring; their full simulator summaries remain available, including
  the uncontrolled tail. Collection part and helper hashes identify the exact
  procedure used for every selected case.

All original attempts and simulator trees are retained in the raw archive
outside Git. Selected compact records preserve the actual run logs, metrics,
original actuator snapshots, source environments and raw-file hashes. Data
from interrupted, failed or smoke runs do not enter the six-case macro score.

## Reproduction

On Linux, using a fresh directory:

```sh
make all test e2e
sudo python3 tests/verify_runtime_forecast.py --output /var/tmp/lerwatt-runtime-fresh
python3 tests/summarize_runtime_forecast.py --input /var/tmp/lerwatt-runtime-fresh --require-gate
```

The helper never reuses or removes an existing simulator root. The scheduler
must be disabled while this isolated test runs. This validates real daemon
code against the standard Linux file ABI of a simulated board; it establishes
neither real-hardware accuracy nor throughput or energy savings.

## Completed results

All six selected cases passed the frozen accuracy and restoration gates.
Equal-profile mean MAE falls 1.473849 to 0.670985 C (-54.47%), and mean RMSE
1.819386 to 0.948024 C (-47.89%). The random and sparse cases retain the
reference exactly. The step-case change is small and should not be presented
as a substantial improvement. Every simulator reports zero throttle steps
and zero invalid writes; each daemon restores all 30 actuator values and
removes its journal.

| Case | Reference MAE C | Temporal MAE C | MAE change | RMSE change | Matched points |
| --- | ---: | ---: | ---: | ---: | ---: |
| period142-passive-30C | 3.530873 | 0.738646 | -79.08% | -76.54% | 26850 |
| sine11-fan-15C | 1.581463 | 0.577625 | -63.48% | -60.58% | 26826 |
| phase-reset66-passive-30C | 1.636611 | 0.616671 | -62.32% | -53.59% | 26853 |
| random7-bare-20C | 1.008802 | 1.008802 | +0.00% | +0.00% | 26850 |
| steps-bare-15C | 0.479315 | 0.478135 | -0.25% | -0.22% | 26853 |
| sparse31-fan-35C | 0.606033 | 0.606033 | +0.00% | +0.00% | 26850 |

The sine case has nine late cluster-point labels, excluded from both columns
under the fixed 250 ms lateness rule. No other selected case has late labels.
All excluded observations remain in the raw data.

[Summary and gate](cases/summary.json), [case sources and environment](cases/environment.json),
and [raw archive SHA256](archives.json) identify the exact data. The first
three cases come from collector `081baec`; the final three from `ec5a696`.
The interrupted and failed attempts are retained under `retained-failures/`
and in the complete raw archive, without changing their original run status.
