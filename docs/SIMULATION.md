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

### Learned policy (`--policy mpc`, experimental)

The learned policy fits a two-node thermal model online. The package is the slow node, fitted by recursive least squares on 10 s means of the package zone (dT_pkg/dt = a·P_total + c·T_pkg + d). The hot spot is an offset above it, T_hot = T_pkg + r·P_own, where r is the ratio of 60 s averages and a learned gain scales it for bursts too short to reach it. A cluster without a package zone (`lerwattd --describe` shows `package (none)`) stays on the rule policy.

Each prediction starts from the measured recent peak and moves it by what the model says changes, so a bias in the model's absolute temperature cancels. The policy steps down when the current ceiling is predicted to cross its line within 5 s. It climbs one OPP only when the next one is predicted to stay under that line for 120 s. A climb that fails doubles the wait before the next one, up to 320 s. The rule policy runs until the model is trained, about 90 s.

The policy holds the filtered zone temperature to soft limit + 2 C (82 C), and each raw reading to 1 C above that. The rule policy steps down at soft limit + 1 C (81 C), but it overshoots that line when the temperature moves fast. The rule cannot simply use the higher line: 1 C more puts its burst peak at 84.1 C, inside the 1 C margin, and 2 C more makes the kernel throttle.

**Result: the learned policy is more stable than the rule, but it does not beat it on work, so the rule stays the default.** Same 900 s cases:

| Scenario | Rule: peak C / moves / served | Learned: peak C / moves / served | Learned vs rule |
| --- | --- | --- | --- |
| Full load, passive | 81.3 / 2 / 68.43% | 82.2 / 2 / 68.75% | +0.32 points of work, one TSADC code hotter |
| Big-core load, passive | 72.1 / 0 / 99.99% | 72.1 / 0 / 99.99% | Equal |
| Full load, bare board | 82.2 / 29 / 51.21% | 82.2 / 10 / 51.35% | +0.14 points, 19 fewer moves |
| Burst load, bare board | 83.2 / 8 / 67.44% | 83.2 / 6 / 67.19% | **-0.25 points**, 2 fewer moves |

The parameters were tuned on those four cases, so `tests/test_closed_loop.c` also runs 19 held-out cases: other sensor-noise seeds, 15 C and 35 C ambient, warm starts, fan cooling, 300 ms and 10 s bursts, load steps and a single busy cluster. Neither policy broke a safety requirement in any of them. The comparison is mixed:

| Across the 19 held-out cases | Learned better | Equal | Learned worse |
| --- | --- | --- | --- |
| Work served | 7 | 7 (within 0.01 points) | 5 (-0.20 to -0.76 points; all bursts) |
| Ceiling moves | 7 | 9 | 3 (+2, +3, +20) |
| Peak temperature | 0 | 16 | 3 (one TSADC code) |

The largest gain in stability is 126 moves down to 27 with 10 s bursts, at a cost of 0.76 points of work. The worst case for stability is a warm-started bare board under 2 s bursts: 30 moves against 10, with 0.31 points less work.

The two-node model fixed what the first-order model got wrong. It recovers the plant: ambient within about 3 C, and a within about 10% of 1/C_die. r comes out about 20% above the simulated resistance, or more on a throttled bare board, because it also absorbs leakage the Energy Model does not count. The 2 s bursts no longer make the policy alternate one OPP (6 moves against 159 before). What remains is a limit of the control, not of the model:

- Under bursts, raw peaks limit the top OPP. Matching the rule's work takes more moves than the rule makes: with a 60 s hold, the burst case serves 67.28% in 23 moves.
- The learned policy's extra work under full load partly comes from its higher line. At that same 82 C line the rule serves more than the learned policy in all three cases that throttle (68.78%, 51.66%, 68.10%), but it is hotter on the bare board and breaks the 1 C margin under bursts.
- Each cluster still decides alone, so the hottest cluster steps down even when another would give up less work per degree. On the bare board the policy ends at the best steady OPP combination an offline search of this model finds. On the passive board it ends at 1.8 / 2.2 / 2.2 GHz, and the search finds 1.6 / 2.4 / 2.4 GHz serves about 1.9 points more at steady state under the same line. Planning across clusters is the next thing to try.

## 5. Calibration, when the board arrives

1. Idle the board at a fixed ambient: measure base power and idle temperatures.
2. Pin each cluster at each OPP under a constant busy loop, one cluster at a time: fit leakage per core and its temperature coefficient from the power drift as the die warms.
3. Apply power steps and fit the two time constants per cluster (hot spot, die) and the die-to-ambient resistance.
4. Write the fitted values into a new platform profile and replay the same traces in the simulator. Acceptance: temperature RMSE under 1.5 C and mean power error under 5% on held-out workloads.
5. From then on the simulator is a calibrated twin of that board, and every result needs a matching board run before it is reported.
