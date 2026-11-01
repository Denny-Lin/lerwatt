# Simulation and the real-machine interface

Status: **Phase 1 testbed, 2026-11-01.** Until an RK3588 board is connected, `lerwatt-sim` stands in for one. When a board is connected, `lerwattd` uses it with no flag and no code change.

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

## 4. Closed-loop results (simulation)

`tests/test_closed_loop.c` runs the real control law against the model for 900 simulated seconds per case, observing only what `lerwattd` can observe on a board.

| Scenario | Controller | Peak C after 300 s | Seconds above 85 C (all zones) | Kernel throttle steps | Ceiling moves | Mean board W | Work served |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Full load, passive | off | 84.1 | 0 | 0 | 0 | 10.61 | 69.4% |
| Full load, passive | Lerwatt | 81.3 | 0 | 0 | 2 | 10.48 | 68.4% |
| Big-core load, passive | off | 72.1 | 0 | 0 | 0 | 8.76 | 100% |
| Big-core load, passive | Lerwatt | 72.1 | 0 | 0 | 0 | 8.76 | 100% |
| Full load, bare board | off | 94.8 | 2293 | 5228 | 0 | 8.17 | 53.4% |
| Full load, bare board | Lerwatt | 82.2 | 0 | 0 | 29 | 7.50 | 51.2% |
| Burst load, bare board | off | 85.9 | 11.5 | 93 | 0 | 6.88 | 68.9% |
| Burst load, bare board | Lerwatt | 83.2 | 0 | 0 | 8 | 6.77 | 67.4% |

"Work served" below 100% under full load is expected: the pattern asks every core for a big core's worth of work, which the A55 cores cannot deliver. On the bare board the kernel's step_wise governor holds the hot spot near the trip by sawtoothing the frequency (5228 steps); Lerwatt keeps it about 3 C under the trip with 29 moves, spending 8% less power for 2.2 points less work. These are simulator results and prove only that the control loop behaves; they are not energy claims.

### First learned policy (`--policy mpc`, experimental)

The learned policy fits a thermal model online (recursive least squares on 5 s averages: dT/dt = a·P_total + c·T + d), steps down when the model predicts the current ceiling will cross the soft limit within 20 s, and steps up only when the next OPP's predicted steady state is under the limit. The rule controller's reactive guard stays in force. Same 900 s cases:

| Scenario | Rule: peak C / moves / served | Learned: peak C / moves / served |
| --- | --- | --- |
| Full load, passive | 81.3 / 2 / 68.4% | 81.3 / 19 / 68.0% |
| Big-core load, passive | 72.1 / 0 / 100% | 72.1 / 0 / 100% |
| Full load, bare board | 82.2 / 29 / 51.2% | 79.5 / 40 / 50.9% |
| Burst load, bare board | 83.2 / 8 / 67.4% | 80.4 / 159 / 66.0% |

The learned policy is safe in every case (no kernel throttling, peaks lower than the rule's), but it is **not better yet**. It serves slightly less work, and under 2 s bursts it alternates one OPP. The estimator is not the cause: `tests/test_learn.c` recovers a first-order plant's parameters through 1 C quantization. The cause is the model structure. The package has two time constants (hot spot about 1 s, die and heatsink hundreds of seconds), and one first-order fit cannot represent both. The next step is a two-node model, with the hot-spot offset fitted from power steps. The rule policy remains the default until a learned policy beats it on served work at equal safety.

## 5. Calibration, when the board arrives

1. Idle the board at a fixed ambient: measure base power and idle temperatures.
2. Pin each cluster at each OPP under a constant busy loop, one cluster at a time: fit leakage per core and its temperature coefficient from the power drift as the die warms.
3. Apply power steps and fit the two time constants per cluster (hot spot, die) and the die-to-ambient resistance.
4. Write the fitted values into a new platform profile and replay the same traces in the simulator. Acceptance: temperature RMSE under 1.5 C and mean power error under 5% on held-out workloads.
5. From then on the simulator is a calibrated twin of that board, and every result needs a matching board run before it is reported.
