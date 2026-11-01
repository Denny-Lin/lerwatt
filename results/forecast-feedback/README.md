# Delayed-feedback forecast comparison

Recorded 2026-11-01 against Lerwatt baseline `7599b99`. This experiment changes point forecasts and their telemetry; it does not change thermal planning, limits or actuation.

The three experts are persistence, the constant-current-power thermal model, and the existing causal-load thermal forecast. A projected normalized LMS blend learns only from predictions whose five-second labels have arrived. Its update step is limited by the 50 outstanding 100 ms feedback intervals. A second, delayed squared-error comparison publishes a simpler expert when it has lower recent error. All published weights are nonnegative and sum to one.

Stored predictions are scored before a new forecast is issued. The queue admits at most 10 forecasts/s, rejects labels over 250 ms late, and resets on clock rollback. There are no future observations, simulator coefficients or workload identities in the forecaster.

The initial hard MAE selector reduced typical error but increased RMSE in several traces; a fast 0.1-step blend regressed on low-duty-cycle bursts. Those designs were rejected. The final step is derived from feedback delay rather than fitted to the reserved workload scores.

## Reserved workload results

Eight profiles were reserved after the final predictor and parameters were fixed. Values are paired on the same model-ready predictions. The macro-average weights profiles equally; it is descriptive, not a hardware result or confidence interval.

| Profile | Old MAE | New MAE | Old RMSE | New RMSE | Model-ready coverage |
| --- | ---: | ---: | ---: | ---: | ---: |
| varying-period-passive-30C-seed104729 | 1.3838 | 1.1864 | 1.7003 | 1.5538 | 100.0% |
| asymmetric-steps-bare-20C-seed130363 | 1.0907 | 0.6159 | 1.6013 | 0.9496 | 100.0% |
| random-fast-bare-35C-seed155921 | 1.2352 | 0.6963 | 1.5878 | 1.1118 | 100.0% |
| sparse-bursts-fan-seed196613 | 1.0422 | 1.0425 | 1.6409 | 1.6410 | 87.2% |
| sine-passive-15C-seed262147 | 0.6726 | 0.6853 | 0.8534 | 0.8690 | 100.0% |
| cooling-change-35C-seed327673 | 0.5225 | 0.4438 | 0.6474 | 0.5866 | 100.0% |
| noisy-burst-bare-seed393241 | 1.5235 | 1.0777 | 1.8195 | 1.3329 | 100.0% |
| duty2of7-bare-seed458879 | 1.2071 | 1.2071 | 1.5079 | 1.5079 | 100.0% |

The equal-profile mean MAE is 1.0847 C before and 0.8694 C after (-19.85%). Five additional predeclared simulator replicates reduce that mean by 18.27% to 21.41%. These are repeated simulator noise/input seeds on the same eight families, not independent hardware trials.

**Negative results:** the sine profile regresses (0.6726 to 0.6853 C MAE); sparse bursts are effectively tied but slightly worse on the matched window. The cooling-change profile remains worse than persistence in MAE despite lower RMSE. Package forecasts are not uniformly improved either. No universal accuracy claim is made.

The sparse-burst physical model is ready for only 87.2% of the scoring window. `all_samples` and `all_*` metrics retain the full window, including persistence fallback: 1.0603 C new MAE versus 1.2192 C persistence across all 26,853 points. Other profiles have full model-ready coverage. Correlated forecasts within a trace are not independent repetitions.

## Runtime verification

A 120 s eight-vCPU Linux run exercised the real BPF contract, trained models, all three clusters, delayed label counts and changing blend weights. Feedback began only after five seconds, weights stayed normalized, and scheduler recovery succeeded. The simulator/controller and regression suite report no new control failures. Independent and joint closed-loop textual results were byte-identical to the baseline evidence.

## Files and reproduction

- `design.json`: frozen parameter/source hashes, reporting-only additions and replicate plan.
- `legacy.jsonl`: six historical profiles used during development.
- `development.jsonl`: six extra development profiles; not an untouched test set.
- `validation.jsonl`: the eight reserved profiles.
- `replicates.jsonl` and `replication-summary.json`: all five additional replicates, including regressions.
- `vm.json`: real VM feedback, weights and scheduler-restoration checks.

```sh
make test
build/test_forecast --legacy
build/test_forecast --development
build/test_forecast --validation
build/test_forecast --validation --replicate 1
make all e2e  # Linux only
```

`hot_mae_c` / `hot_rmse_c` retain the old causal-model comparator. `adaptive_*` fields describe the deployed delayed-feedback forecast. `all_*` fields include periods when the physical model is unavailable. Replicate zero uses the original seed encoded in the case ID; nonzero replicates change the explicit JSON `seed`, while retaining the profile ID.
