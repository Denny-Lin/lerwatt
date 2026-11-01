# Simulation and the real-machine interface

Validation recorded **2026-11-01**. Until an RK3588 board is connected, `lerwatt-sim` stands in for one. When a board is connected, `lerwattd` uses it with no flag and no code change.

## 1. Where the cut is

The cut is the Linux kernel ABI. `lerwattd` reads and writes only what a real kernel exposes: sysfs, procfs, debugfs and hwmon. Every path is resolved under a *sysroot*. On a board the sysroot is `/`. `lerwatt-sim` is a separate program that maintains the same files under its own root and moves them like hardware would.

| Part | In the VM today | On a board | Code |
| --- | --- | --- | --- |
| Scheduler (EEVDF or Lertick), tasks, their CPU time | **Real** | Real | the kernel |
| Lertick contract maps on bpffs | **Real** (never simulated) | Real | `include/lerwatt/contract.h`, `src/contract.c` |
| `lerwattd`: discovery, sampling, control law, writes, journal | **Real** | Real | `src/` |
| OPP clocking and voltage, power, idle-state power | Simulated | Real silicon | `sim/model.c` |
| Thermal network, TSADC sensors, INA226 meter | Simulated | Real sensors | `sim/model.c` |
| cpufreq core, schedutil, freq QoS, step_wise, critical trip | Emulated | Real kernel | `sim/model.c` |
| The ABI files themselves | Fake tree under `/run/lerwatt-sim` | `/sys`, `/proc` | `sim/abi.c` |

No simulator object is linked into `lerwattd` (see the `Makefile`). The only things both programs share are the contract header and, in tests only, the pure control law.

## 2. How the switch happens

`lerwattd` picks its sysroot in this order (`src/hal_discover.c`, `lw_detect()`):

1. `--sysroot DIR` if given.
2. This machine, if `/sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq` exists. Real hardware always wins.
3. A running simulator: `$LERWATT_SIM_ROOT` or `/run/lerwatt-sim`, recognized by its `.lerwatt-sim` marker.
4. Otherwise it exits with a message telling you to start the simulator.

The simulator writes the marker last and deletes it on exit, so `lerwattd` never attaches to a half-built or stopped tree. It refuses `/`, `/sys` and `/proc` as its root.

A separate `.lerwatt-sim-root` identity remains after exit, allowing the same
root to restart without `--force`. The simulator holds an exclusive lock on
that file while running; a second simulator cannot take over the root, even
with `--force`. ABI files, unrelated files and recovery journals are retained.
Older non-empty roots without either marker are still rejected: inspect and
preserve them before selecting a fresh root. The persistent identity alone
does not make a stopped simulator discoverable by `lerwattd`.

```sh
make && make test && make e2e

# In the VM (no cpufreq): start the simulated board, then the controller.
sudo build/lerwatt-sim --load procstat --cooling passive &
sudo build/lerwattd --telemetry lerwatt.jsonl

# On the board: the same command; it finds /sys by itself.
sudo build/lerwattd --telemetry lerwatt.jsonl
```

`lerwattd --describe` prints which mode it chose and what it found.

## 3. Fidelity

### Exact, from Linux v7.3

| Item | Source |
| --- | --- |
| OPP frequencies and voltages, 3 clusters | `arch/arm64/boot/dts/rockchip/rk3588-opp.dtsi` |
| `capacity-dmips-mhz` 530 / 1024, `dynamic-power-coefficient` 228 / 416 | `rk3588-base.dtsi` |
| CPU capacity normalization (little = 397) | `drivers/base/arch_topology.c` |
| Energy Model power, performance, cost, inefficiency | `drivers/opp/of.c`, `kernel/power/energy_model.c` |
| `cpu-sleep` idle state: 220 us latency, 1000 us residency | `rk3588-base.dtsi` |
| Thermal zones, passive 85 C (2 C hysteresis), critical 115 C, cooling maps | `rk3588-base.dtsi` |
| TSADC code table and its truncating interpolation (about 0.92 C per code) | `drivers/thermal/rockchip_thermal.c` |
| TSADC sampling every 250 ms, 50 ms when hot | same |
| step_wise target-state logic and trip hysteresis | `drivers/thermal/gov_step_wise.c` |
| schedutil: util x 1.25, lowest OPP at or above the target, freq QoS limits | `kernel/sched/cpufreq_schedutil.c`, `drivers/cpufreq/cpufreq.c` |
| `pm_qos_resume_latency_us` semantics: `"0"` none, `"n/a"` zero | `drivers/base/power/sysfs.c` |

`tests/test_model.c` pins these numbers.

### Assumed, to be calibrated on a board

| Parameter | Value | Why it matters |
| --- | --- | --- |
| Leakage per core at top voltage and 85 C | A55 0.04 W, A76 0.20 W | Power at high temperature |
| Leakage temperature coefficient | 0.02 /K | Thermal runaway margin |
| Cluster hot spot to die | A55 2.5 K/W, 0.4 J/K; A76 3.0 K/W, 0.3 J/K | Fast temperature swings |
| Die to ambient | passive 7.5 K/W, fan 3.0 K/W, bare 14 K/W | Steady-state temperature |
| Die heat capacity | 30 J/K (8 J/K bare) | Minutes-scale warm-up |
| Board base power, SoC idle heat, DRAM at full load | 2.2 W, 0.6 W, 0.6 W | Measured board power |
| Rail efficiency | 0.88 | Measured board power |
| Sensor noise | 0.3 C | Controller jitter |
| Deep idle share of idle time | 85% below 50% load, falling with load | Idle power |

### Approximations and ABI deviations

- PELT is a 32 ms exponential average, not the kernel's segmented sum.
- The VM's real busy time becomes demand on the simulated core, scaled by its speed (`--sref`, default: a host vCPU equals an A76 at 2.4 GHz). Work above the simulated capacity is counted as not served. **The VM's tasks do not actually slow down**, so latency measured inside the VM says nothing about the simulated board. Use the simulator for power, temperature and control behaviour, and the board for latency.
- A file `lerwattd` writes (`scaling_max_freq`, `scaling_min_freq`, `pm_qos_resume_latency_us`, `cpuidle/stateN/disable`) reads back what was written. The real kernel reads back the effective cpufreq limit. `lerwattd` never relies on reading those back, except to record originals at start-up.
- With Lertick attached, schedutil follows Lertick's `cpuperf_max` per cluster (what happens when sched_ext runs every task); otherwise it follows PELT utilization.
- The VM should have 8 vCPUs (`-smp 8`) so that every simulated core gets real demand. With fewer, use `--cpu-map`.

## 4. Control and forecast results

These are simulator measurements, not hardware energy or latency claims. Reproduce with:

```sh
make test
build/test_closed_loop
build/test_closed_loop --cross-cluster
build/test_forecast
```

Full textual outputs are preserved in [results/thermal-control](../results/thermal-control/README.md).

### Equal temperature settings

All controllers now use the same 80 C soft limit, 81 C filtered decision line
and raw-temperature guard at 82 C (the passive trip minus 3 C). MPC no longer
uses the earlier, higher 82 C filtered line. The guard immediately caps to
the configured minimum ceiling, rather than waiting for the temperature EMA
or the normal one-OPP-per-second descent. Both policies retain the kernel's
85 C passive trip and 115 C critical trip. TSADC quantization, noise and its
250 ms sampling delay mean the guard must act before the desired 84 C peak.
It cannot guarantee that arbitrary hardware or a controller started after
overheating will meet that peak.

### Hot startup

The original 79 C hot-start reproduction reached 85.9 C and triggered thermal
throttling even without Lertick or contract startup delays: the controller
began at the highest ceiling and reacted too slowly to the rising hot spot.
A first guard at 83 C eliminated throttling but still peaked at 84.1 C, failing
the one-degree margin; it was not accepted.

The final tests include full and two-second burst loads from 79 C, three
cooling profiles and seeds 42/7/99, for both policies in each comparison.
All 72 startup runs across the independent and joint comparisons passed:
maximum sensor temperature 83.2 C, no kernel throttle steps. The original four
900 s cases and 19 regression cases now check the one-degree peak margin from
time zero as well as reporting peaks after 300 s.

The protection has a cost. Relative to the earlier rule-controller report,
full bare-board work served fell from 51.21% to 51.04%, and two-second burst
work fell from 67.44% to 66.43%; burst ceiling moves rose from 8 to 42. These
changes are reported rather than hidden by raising the temperature line.

### Independent and cross-cluster control

The learned policy fits a package node with recursive least squares and a
per-cluster hot-spot offset. Independent MPC predicts a five-second heating
horizon and requires an increase to fit over a 120 s hold. It uses rule control
until trained, usually around 90 s, or when no package sensor is available.

`--policy mpc --cross-cluster` enables bounded enumeration of discovered OPP
combinations. It maximizes estimated work among combinations predicted to fit
both horizons for every cluster, using total package power and each cluster's
own hot-spot power. If none fits, it minimizes the largest predicted violation.
A plan is held for 120 s; the shared raw guard can still override it immediately.
A first version re-planned every 10 s and failed stability checks, so that
version was rejected. Missing sensors, Energy Models, untrained models, a
single cluster or more than 65,536 combinations retain independent control.
Inputs are sampled together before any cluster's estimate is updated.

No simulator thermal constants or offline optimal OPP combinations enter the
planner. The offline estimate of roughly 1.9 additional points under passive
cooling is not an achieved online result.

Each case lasts 900 simulated seconds. Cells show **peak C after 300 s /
ceiling moves / work served**; startup-inclusive safety is checked separately.

| Case | Rule | Independent MPC | Joint MPC |
| --- | --- | --- | --- |
| full load, passive | 81.3 / 2 / 68.43% | 81.3 / 2 / 68.38% | 81.3 / 5 / 68.44% |
| big-core load, passive | 72.1 / 0 / 99.99% | 72.1 / 0 / 99.99% | 72.1 / 1 / 99.99% |
| full load, bare board | 81.3 / 11 / 51.04% | 81.3 / 14 / 51.08% | 81.3 / 8 / 50.89% |
| burst load, bare board | 82.2 / 42 / 66.43% | 82.2 / 34 / 66.52% | 79.5 / 8 / 65.56% |

Original 19 held-out cases, retained as regression cases after inspection:

| Case | Rule | Independent MPC | Joint MPC |
| --- | --- | --- | --- |
| full passive, seed 7 | 81.3 / 2 / 68.44% | 81.3 / 4 / 68.35% | 81.3 / 2 / 68.19% |
| full bare, seed 7 | 81.3 / 10 / 51.04% | 81.3 / 16 / 51.09% | 81.3 / 10 / 50.89% |
| burst bare, seed 7 | 82.2 / 39 / 66.51% | 82.2 / 27 / 66.68% | 79.5 / 9 / 65.96% |
| burst bare, seed 99 | 82.2 / 38 / 66.62% | 82.2 / 34 / 66.49% | 79.5 / 8 / 65.75% |
| full passive, 35 C | 82.2 / 41 / 62.60% | 81.3 / 11 / 62.95% | 81.3 / 7 / 62.97% |
| full bare, 35 C | 82.2 / 21 / 44.77% | 81.3 / 12 / 44.38% | 81.3 / 20 / 44.37% |
| burst bare, 35 C | 82.2 / 151 / 59.24% | 82.2 / 13 / 60.25% | 79.5 / 12 / 58.95% |
| full bare, 15 C | 81.3 / 32 / 55.73% | 79.5 / 10 / 55.22% | 80.4 / 6 / 55.13% |
| full passive, warm 60 | 81.3 / 2 / 67.18% | 81.3 / 6 / 67.04% | 81.3 / 6 / 66.85% |
| full bare, warm 70 | 82.2 / 51 / 48.98% | 81.3 / 13 / 49.23% | 82.2 / 21 / 47.98% |
| burst bare, warm 70 | 82.2 / 61 / 65.23% | 82.2 / 58 / 65.27% | 81.3 / 15 / 64.76% |
| 10 s bursts, bare | 82.2 / 110 / 66.05% | 81.3 / 27 / 65.87% | 77.6 / 8 / 64.50% |
| 300 ms bursts, bare | 69.3 / 0 / 58.03% | 69.3 / 0 / 58.03% | 69.3 / 0 / 58.03% |
| 10 s bursts, passive | 60.1 / 0 / 70.46% | 60.1 / 0 / 70.46% | 60.1 / 0 / 70.46% |
| load steps, bare | 82.2 / 31 / 69.42% | 81.3 / 30 / 69.83% | 81.3 / 15 / 68.30% |
| load steps, passive | 69.3 / 0 / 79.25% | 69.3 / 0 / 79.25% | 61.0 / 18 / 73.56% |
| one cluster, bare | 76.7 / 0 / 99.99% | 76.7 / 0 / 99.99% | 76.7 / 2 / 99.99% |
| full, fan | 52.7 / 0 / 69.38% | 52.7 / 0 / 69.38% | 52.7 / 0 / 69.38% |
| burst, fan, 35 C | 52.7 / 0 / 68.92% | 52.7 / 0 / 68.92% | 52.7 / 0 / 68.92% |

**The rule remains the default; joint planning is opt-in.** Neither learned
configuration consistently improves both work and stability. In particular,
joint MPC sacrifices work on several long-burst and load-step cases despite
reducing switching. The parameters were not swept to optimize the original
19 cases. Those cases are now regression evidence rather than an untouched
source of generalization claims.

### Eight-vCPU Linux integration

The same `procstat` and BPF-map setup was re-run with eight pinned busy workers:

| Run | Initial temperature | Duration | Joint activation | Sensor peaks big0 / big1 / little | Kernel throttle steps |
| --- | --- | --- | --- | --- | ---: |
| Rule | 79 C | 25 s scheduler run | disabled | 83.2 / 83.2 / 82.2 C | 0 |
| MPC + joint, hot | 79 C | 140 s scheduler run | models not ready; rule fallback | 82.2 / 83.2 / 82.2 C | 0 |
| MPC + joint, cold | 25 C | 240 s scheduler run | all clusters active from daemon t=90.6 s | 80.4 / 82.2 / 80.4 C | 0 |

The hot-joint harness initially required activation within 140 s and failed
that assertion. It confirmed conservative fallback, not joint execution.
The separate cold run exercised trained joint decisions for 1,519 control
periods. Models were not force-enabled or given simulator coefficients.
All runs retained valid contract/demand map records, reported zero bad knob
writes and restored the default scheduler. Simulation summaries include a
small amount of startup/shutdown time beyond each finite scheduler run.
These runs have different lengths and initial conditions, so their work-served
figures are not a policy comparison. Exact summaries are preserved in
[vm.json](../results/thermal-control/vm.json).

### Prediction accuracy, measured independently of work served

`test_forecast` saves five-second point forecasts before their future labels
exist, and scores them when those labels arrive. The workload runs for 1200 s.
These traces exercise the model module under simulated kernel policies,
without Lerwatt control or Lertick. They do not measure the daemon's closed-loop
forecast error or real-hardware accuracy.
Forecasts start after 300 s, producing 26,853 cluster-label pairs per trace.
The model continues learning causally; no future samples are used to build a
forecast. These overlapping, correlated forecasts are descriptive metrics,
not independent trials or confidence intervals.

A constant-current-power forecast overestimates how long a short burst lasts.
The point forecaster uses a causal 30 s power mean after at least three recent
power-direction reversals, while retaining current power for isolated changes.
This changes the forecast input, not the RLS coefficients or the planner's
safety prediction. Telemetry separates `forecast_c` (expected temperature in
five seconds) from `pred_c` (planning prediction), with `-1` for unavailable.

The baseline predicts that the current sensor temperature persists. The
constant-power comparator uses the same learned model at current power. All
metrics below are hot-spot MAE in C; lower is better.

| Trace | Persistence | Constant power | Causal forecast |
| --- | ---: | ---: | ---: |
| steps-passive-seed7 | 0.4779 | 0.5406 | 0.5406 |
| burst2-bare-seed99 | 1.8220 | 3.3919 | 1.4259 |
| burst03-fan-35C | 0.5151 | 2.2938 | 0.5163 |
| steps-bare-15C-seed13579 | 0.6895 | 0.6020 | 0.7202 |
| burst2-passive-35C-seed2026 | 1.8265 | 2.1709 | 1.3909 |
| burst03-bare-seed2718 | 0.4366 | 3.4102 | 0.4766 |

The last three traces use additional seeds/environments reserved after the
forecasting rule was fixed; no thresholds were changed in response to those
results. The model beats persistence on the two 2 s burst traces, while the
other four are worse, including a nearly tied 300 ms fan trace. The 15 C step
trace also regresses against constant-power forecasting. Package MAE and
hot-spot RMSE are included in the raw JSONL. **A general prediction-accuracy
advantage is unproven.** Future model changes can be compared against these
recorded baselines without confusing lower forecast error with higher throughput.

### Delayed-feedback point forecasts

The current `forecast_c` uses an online, bounded blend of persistence,
constant-power and causal-load model forecasts. It learns from stored
five-second predictions only after the corresponding sensor labels arrive;
a delayed squared-error comparison can retain a simpler expert instead.
Thermal planning still uses `pred_c`, independent of this point forecast.
`forecast_source`, `forecast_weights`, `forecast_labels` and `forecast_dropped`
make fallback, calibration and late-label handling visible in telemetry.
An untrained physical model now still provides a persistence point forecast.

The original six development profiles all improve in MAE and RMSE relative to
the previous causal forecaster. On eight reserved new profiles, equal-profile
mean hot-spot MAE falls from 1.0847 C to 0.8694 C (19.85%). Five predeclared
replicates show 18.27% to 21.41% reductions in that aggregate. The sine profile
and sparse-burst matched window regress slightly; cooling-change MAE still
loses to persistence, and package error is mixed. A model-ready coverage gap
in sparse bursts is reported with full-window fallback metrics rather than
silently excluded.

These are causal simulator-module forecasts, not real-board or daemon
closed-loop accuracy claims. No parameters were changed after inspecting the
eight reserved profiles. Full tables, all replicates, rejected designs and
runtime checks are in [the forecast report](../results/forecast-feedback/README.md).
The independent and joint closed-loop control outputs remain byte-identical
to the earlier control evidence; the rule remains the default.

## 5. Calibration, when the board arrives

1. Idle the board at a fixed ambient: measure base power and idle temperatures.
2. Pin each cluster at each OPP under a constant busy loop, one cluster at a time: fit leakage per core and its temperature coefficient from the power drift as the die warms.
3. Apply power steps and fit the two time constants per cluster (hot spot, die) and the die-to-ambient resistance.
4. Write the fitted values into a new platform profile and replay the same traces in the simulator. Acceptance: temperature RMSE under 1.5 C and mean power error under 5% on held-out workloads.
5. From then on the simulator is a calibrated twin of that board, and every result needs a matching board run before it is reported.
