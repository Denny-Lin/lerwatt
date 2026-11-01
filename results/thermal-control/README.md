# Thermal control and forecast evidence

Recorded 2026-11-01 from the accompanying source. These files contain simulator results, not hardware measurements.

- `independent.txt`: rule versus independent MPC, four main scenarios, 19 regression scenarios and 36 startup runs.
- `joint.txt`: rule versus joint MPC with the same temperature settings, cases and startup runs.
- `vm.json`: eight-vCPU hot rule, hot joint fallback and cold trained-joint integration outcomes, including raw simulator/controller summaries and map records.
- `forecast.jsonl`: prospective five-second point forecasts, persistence and constant-power comparators; package MAE, hot-spot MAE and RMSE. Samples within a trace overlap and are correlated.

Reproduce on Linux or macOS:

```sh
make test
build/test_closed_loop > independent.txt
build/test_closed_loop --cross-cluster > joint.txt
build/test_forecast > forecast.jsonl
```

Linux additionally runs `make all e2e`. The detailed conditions, limitations and regressions are in [Simulation](../../docs/SIMULATION.md). No performance-based test asserts that the learned controller or forecaster must win; safety, stability, fallback and numerical checks remain mandatory.
