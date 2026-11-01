# Temperature-history point forecasts

Recorded 2026-11-01 against the deployed delayed-feedback forecaster at
`c7607d4`. These are prediction-accuracy results on unchanged simulated
controller trajectories, not a throughput, hardware or safety improvement.

## Method

The seven features are a bias, raw temperature differences at 1, 2, 5 and
10 seconds, and the existing constant-power/causal-model forecasts minus
the current sensor. A bounded normalized LMS model learns a correction to
the deployed reference forecast only when its five-second label arrives.
Its step is derived from the 50 outstanding 100 ms feedback intervals.

A 30-second paired squared-error history gates the correction. The advantage
must exceed a variability margin using six five-second blocks; this is a
conservative heuristic, **not a statistical confidence or coverage claim**.
Coefficients stay within +/-4, input changes within +/-20 C, training residuals
within +/-10 C and output corrections within +/-20 C. Missing, stale or
non-finite observations retain the reference. Thermal planning and all limits
still use the original filtered control model.

An independent raw-temperature thermal fit did not improve the six-profile
development mean MAE (0.630826 to 0.631623 C); it was rejected. Early temporal
hard selectors, a blend, and an unguarded residual learner also regressed on
asymmetric or slowly changing traces. Those attempts and all development
outputs are retained. The first eight temporal confirmation cases became
development after the early selector failed; they are not the final test set.

## Fresh confirmation

The final learner and variability guard were fixed before 32 new profiles
were scored. Six predeclared noise/input seeds yield 192 complete profiles.
Each runs for 1200 seconds, with scoring issued after 300 seconds. All
26,853 matched cluster forecasts per profile include fallback; no physical-
model readiness filter removes difficult periods.

On replicate zero, equal-profile mean MAE falls from 0.966472 to 0.681972 C
(-29.44%) and mean RMSE from 1.272395 to 0.961352 C (-24.45%). Across all six
seeds, MAE reductions range from 28.95% to 29.51%, and RMSE reductions from
23.87% to 24.45%. These are macro-averages, not per-point confidence intervals.
Different datasets were used in the earlier forecast study; the percentage
reductions must not be added or multiplied into a combined claim.

The largest gains are on recurring loads; several random, sparse and slow
profiles retain the reference unchanged. **Negative result:** steps-bare-15C
regresses by 0.13% MAE and 0.23% RMSE averaged over seeds; its worst single
replicate regresses by 0.28% and 0.45%. No universal accuracy claim is made.

Values below average the six seeds for each fixed profile:

| Profile | Reference MAE C | Temporal MAE C | MAE change | RMSE change |
| --- | ---: | ---: | ---: | ---: |
| asymmetric-bare-35C | 0.5134 | 0.5134 | +0.00% | +0.00% |
| asymmetric-fan-25C | 0.6016 | 0.6016 | +0.00% | +0.00% |
| asymmetric-passive-15C | 0.6121 | 0.6121 | +0.00% | +0.00% |
| cooling-change-20C | 0.4347 | 0.4263 | -1.93% | -3.10% |
| cooling-change-30C | 0.4445 | 0.4277 | -3.78% | -4.96% |
| period022-passive-25C | 0.3904 | 0.3904 | -0.00% | -0.00% |
| period09-fan-25C | 0.4826 | 0.4816 | -0.19% | -0.19% |
| period14-bare-35C | 0.5905 | 0.4446 | -24.71% | -21.58% |
| period142-passive-30C | 3.4943 | 0.6830 | -80.45% | -78.29% |
| period26-passive-15C | 0.5935 | 0.5855 | -1.34% | -1.16% |
| period38-bare-15C | 2.4659 | 2.4659 | +0.00% | +0.00% |
| period58-bare-25C | 1.0697 | 0.6162 | -42.40% | -36.96% |
| period62-fan-30C | 1.1698 | 1.1698 | +0.00% | +0.00% |
| period82-fan-35C | 1.7980 | 0.6064 | -66.27% | -62.47% |
| phase-reset34-bare-25C | 1.2388 | 0.6808 | -45.04% | -36.17% |
| phase-reset66-passive-30C | 1.6367 | 0.6232 | -61.92% | -51.97% |
| random07-passive-35C | 0.7001 | 0.7001 | +0.00% | +0.00% |
| random23-fan-25C | 0.5902 | 0.5902 | +0.00% | +0.00% |
| random61-passive-30C | 0.4806 | 0.4806 | +0.00% | +0.00% |
| random7-bare-20C | 0.9690 | 0.9690 | +0.00% | +0.00% |
| sine11-fan-15C | 2.3078 | 0.6860 | -70.27% | -68.65% |
| sine113-passive-25C | 0.5911 | 0.5911 | +0.00% | +0.00% |
| sine191-bare-20C | 0.4943 | 0.4943 | +0.00% | +0.00% |
| sine41-bare-35C | 0.8391 | 0.8391 | +0.00% | +0.00% |
| sparse13-bare-20C | 1.0327 | 1.0327 | +0.01% | +0.00% |
| sparse31-fan-35C | 0.5507 | 0.5507 | +0.00% | +0.00% |
| sparse7-passive-15C | 0.9531 | 0.4287 | -55.02% | -53.19% |
| steps-bare-15C | 0.4767 | 0.4773 | +0.13% | +0.23% |
| steps-fan-25C | 0.4638 | 0.4638 | +0.00% | +0.00% |
| steps-passive-35C | 0.4970 | 0.4970 | -0.00% | -0.00% |
| varying-duty46-bare-15C | 0.7582 | 0.7153 | -5.65% | -3.52% |
| varying-duty94-fan-35C | 1.7253 | 1.0655 | -38.24% | -29.53% |

All 192 controller runs retain zero kernel throttling and a startup-inclusive
10 ms sensor peak of at most 83.2 C (passive trip 85 C). The original independent
and joint closed-loop reports are byte-identical. This establishes unchanged
simulated control, not a new control benefit.

## Runtime and telemetry

The module uses 7,344 bytes per cluster in userspace. Training/inference for
this temperature correction runs in lerwattd; Lertick burst learning remains
inside eBPF. Base mixture weights still describe `forecast_base_c`; the separate
`forecast_correction_c` produces the final `forecast_c`. `forecast_source` is
`temporal` when the correction is selected. `temporal_labels` and
`temporal_dropped` report matured and rejected feedback. `t_ns` and three-decimal
sensor/point values permit exact timestamp alignment without 0.1 C rounding.

`--forecast adaptive` retains the previous reference; `--forecast temporal`
is the new point-forecast default for MPC. The default control policy is still
rule. No point forecast changes ceilings, floors, planning or safety limits.

Real daemon/file-ABI confirmation is required separately from these simulator
module scores; no real-board accuracy is claimed. The runtime harness retains
every temporary simulator tree and tests every original actuator restoration.

## Reproduction

```sh
make test
build/test_control_forecast --temporal --integrated --reserved
build/test_control_forecast --temporal --integrated --reserved --replicate 1
# Linux only; six 20-minute cases, using fresh isolated simulator roots:
sudo python3 tests/verify_runtime_forecast.py --output /var/tmp/lerwatt-runtime-forecasts-new
```

`temporal-design.json` records the frozen sources/acceptance criteria. The
post-freeze stale-feedback check changes no perfectly sampled score; integrated
results match all original scores exactly. The 10 ms peak reporting audit also
changes no predictor or control outcome. Both original and audited reports
are retained. The three scoring fixtures verify nanosecond alignment, delayed
labels, rejection of inconsistent corrections and late-label handling.

Rejected prototype sources were captured against the reference revision and
its source hashes; they are evidence, not build targets against the current
controller, which already includes the temporal correction.
