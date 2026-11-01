# Lerwatt design

Status: **2026-11-01.** The contract layout (section 3), the rule-based controller (section 4) and the safety envelope (section 5) are implemented and run against the simulator ([SIMULATION.md](SIMULATION.md)). Learning, Lertick's side of the contract and any hardware run are not done yet. Source references point to the [Linux v7.3 tag](https://github.com/torvalds/linux/tree/v7.3) and to [sched-ext/scx](https://github.com/sched-ext/scx) as of 2026-10-30.

Labels: **[upstream]** = present in Linux v7.3; **[RFC]** = posted to a mailing list, not merged; **[proposal]** = Lerwatt design, not yet tested.

## 1. Principle: replace policy, keep mechanism

Linux spreads CPU power policy across schedutil, EAS, cpuidle governors, thermal governors, powercap and driver preferences. Only two of those decide on every event: the frequency request in schedutil and the idle-state choice in the cpuidle governor. Everything else acts on tens of milliseconds or slower, which a userspace controller can own.

| Policy | v7.3 location | Lerwatt treatment |
| --- | --- | --- |
| Utilization-to-frequency mapping and 1.25× headroom | `sugov_get_util()`, `map_util_perf()` [upstream] | **Replaced.** With sched_ext running all tasks, schedutil starts from `scx_cpuperf_target()` and ignores fair-class utilization. Lertick sets that target. |
| Wake-up placement by energy (EAS) | `find_energy_efficient_cpu()` [upstream] | **Replaced** by Lertick's BPF placement for sched_ext tasks. |
| Power-allocator (IPA) PID and power split | `gov_power_allocator.c` [upstream] | **Replaced** by Lerwatt's budget controller; the kernel's critical trips stay armed. |
| Cluster power capping (DTPM) | `dtpm_cpu.c` `set_pd_power_limit()` [upstream] | **Replaced** by the same Energy-Model-to-ceiling mapping inside Lerwatt (RK3588 has no upstream DTPM hierarchy). |
| Frequency floors and ceilings | frequency QoS, `scaling_{min,max}_freq` [upstream] | **Owned** by Lerwatt. |
| Idle depth | `cpuidle_governor_latency_req()`, `stateN/disable` [upstream] | **Constrained** by Lerwatt; per-entry choice stays in menu/teo. |
| Per-entry idle-state choice | `menu_select()`, `teo_select()` [upstream] | **Untouched.** A BPF cpuidle governor exists only as an [RFC](https://lwn.net/Articles/1035643). |
| Voltage | `_set_opp()` in `drivers/opp/core.c`, firmware [upstream] | **Untouched.** Voltage follows the chosen OPP. |
| Critical and hot trips, hardware throttling | `handle_critical_trips()`, `hw_pressure` [upstream] | **Untouched.** |

## 2. Two controllers, two timescales

| | Lertick (scheduling) | Lerwatt (power) |
| --- | --- | --- |
| Runs in | sched_ext BPF, with a userspace loader | Userspace daemon |
| Period | Every scheduling event; model updates per completed burst | 100 ms control loop; budgets re-planned every 1 s |
| Decides | Placement, slices, per-CPU `cpuperf` target | Per-cluster ceilings and floors, energy prices, idle-latency limits, EPP |
| Observes | Task bursts, queue depth, wake latency | Temperatures, measured energy, frequency residency, Lertick's demand forecast |

The 10× to 100× separation is deliberate. Lerwatt sees Lertick's behaviour as a fast inner loop that has already settled, and Lertick sees Lerwatt's prices as constants between updates. This is the standard condition for stable hierarchical control, and it keeps the two learners from chasing each other.

## 3. The contract [proposal]

Lertick owns two pinned BPF array maps under `/sys/fs/bpf/lertick/`. Both are created with `BPF_F_MMAPABLE` so Lerwatt reads and writes them through `mmap()` without system calls on the hot path. A sequence counter guards each record.

**`lw_contract` (written by Lerwatt, read by Lertick per event), one record per cluster:**

| Field | Meaning |
| --- | --- |
| `seq` | Even when stable, odd while being written |
| `ceiling_cap` | Highest capacity Lertick may request on this cluster (0–1024 scale) |
| `floor_cap` | Lowest capacity to request while latency-critical work runs |
| `energy_price` | Cost per unit of capacity, fixed-point; rises when the cluster is over budget |
| `thermal_headroom` | Degrees below Lerwatt's own soft limit |
| `valid_until_ns` | Lertick ignores the record after this time and falls back to defaults |

**`lt_demand` (written by Lertick, read by Lerwatt each period), one record per cluster:**

| Field | Meaning |
| --- | --- |
| `seq` | As above |
| `pred_util` | Predicted utilization for the next 100 ms |
| `lat_crit_load` | Share of demand from latency-critical tasks |
| `requested_cap` | Mean `cpuperf` target Lertick set in the last period |
| `p99_wake_ns` | Recent wake-latency estimate |

Coordination follows dual decomposition. Lertick minimizes latency cost plus `energy_price × energy` within each cluster's ceiling. Lerwatt adjusts `energy_price` by the gap between measured and budgeted power, and lowers `ceiling_cap` only when the price alone fails to hold the budget. Commands flow one way per field, so the two controllers never write the same knob.

## 4. Algorithms

**Lertick side.** Keep the existing per-task burst predictor. Add capacity-aware placement and a per-CPU `cpuperf` target computed from predicted demand, the contract ceiling and the energy price. The BPF verifier forbids floating point, so all arithmetic stays fixed-point, as Lertick's Q16 model already is.

**Lerwatt, rule-based (implemented in `src/controller.c`).** A budget controller with hysteresis: a thermal model fitted offline (first-order RC per cluster), an Energy-Model power estimate corrected by measured energy, and a PI loop on the price. This is the baseline any learned version must beat.

**Lerwatt, Phase 3 (learning).** A contextual bandit chooses among a small set of safe ceiling and idle-limit profiles per period; the context is temperature, headroom and Lertick's forecast. A model-predictive variant plans over the learned thermal model. Two independent reinforcement-learning agents are not planned: they face a non-stationary environment created by each other. If reinforcement learning is used later, it will be trained centrally with a shared reward and executed separately, with the contract fields as the only shared state.

**Shared objective for evaluation:** energy-delay product, subject to a p99 latency target and a temperature limit.

## 5. Safety envelope and fallback [proposal]

1. Allowed writes: `scaling_min_freq`, `scaling_max_freq`, `energy_performance_preference`, `power/pm_qos_resume_latency_us`, `cpuidle/stateN/disable`, and the contract map. Nothing else. The current code uses the first two cpufreq knobs, the resume-latency knob and the contract; it never loosens a limit it found in place.
2. Every written value is clamped to the hardware range and to a configured minimum performance floor.
3. Original values are saved at start and restored on exit, on signals, and by a service-manager stop hook if the daemon dies.
4. If `valid_until_ns` expires, Lertick reverts to its own defaults; if Lertick unloads, schedutil returns to fair-class utilization automatically.
5. The kernel's passive and critical trips remain armed. On RK3588 the upstream device tree sets passive trips at 85 °C and critical trips at 115 °C; Lerwatt's soft limit sits below the passive trip.

## 6. Interfaces and gaps

All interfaces Lerwatt needs exist in v7.3: `scx_bpf_cpuperf_set()` and `scx_bpf_cpuperf_cap/cur()` in `kernel/sched/ext/ext.c`; frequency QoS through cpufreq sysfs; device resume-latency QoS and cpuidle state disable through sysfs; thermal zones through sysfs and thermal netlink; the Energy Model through debugfs or the energy-model generic-netlink family in `kernel/power/em_netlink.c`.

Known gaps, none blocking:

- sched_ext does not update `rq->cpu_capacity`, so a BPF scheduler must read `cpufreq_cpu_data` and `hw_pressure` itself; scx_lavd does this with `__ksym` in `power.bpf.c`. A small kfunc would be cleaner.
- RK3588 has no upstream DTPM hierarchy; only RK3399 does (`drivers/soc/rockchip/dtpm.c`).
- BPF frequency governor ([cpufreq_ext](https://www.phoronix.com/news/cpufreq_ext-RFC), 2024) and BPF cpuidle governor ([2025](https://lwn.net/Articles/1035643)) remain [RFC].

## 7. Hardware and measurement

- **Primary board:** RK3588 (4× Cortex-A55 with `capacity-dmips-mhz` 530 and 4× Cortex-A76 with 1024 in the upstream device tree, three OPP tables, an Energy Model from `dynamic-power-coefficient`, cluster thermal zones). Linux drives voltage through `cpufreq-dt` and regulators, so the full OPP path is visible.
- **Second platform (later):** a Qualcomm board with hardware DVFS and LMh throttling, to test firmware-owned voltage and `hw_pressure`.
- **Energy:** an external meter on the board's input rail, sampled at 1 kHz or faster and time-aligned with traces. The Energy Model is an estimate, not a measurement.
- **Thermal:** fixed ambient, fixed heatsink and fan setting, cool-down to a fixed start temperature before each run.

## 8. Evaluation plan

| # | Scheduler | Power policy |
| --- | --- | --- |
| 1 | EEVDF | schedutil + EAS + kernel thermal governor |
| 2 | Lertick | Kernel policies (schedutil reading Lertick's `cpuperf`) |
| 3 | EEVDF | Lerwatt (ceilings, idle limits, budgets) |
| 4 | Lertick | Lerwatt, contract disabled (no prices, no forecasts) |
| 5 | Lertick | Lerwatt, contract enabled |

scx_lavd in its powersave and autopilot modes is an additional baseline because it already combines core compaction with `cpuperf` control.

Metrics: throughput, p95/p99 latency, energy, energy-delay product, frequency transitions (`power:cpu_frequency` tracepoint), time above the passive trip, `hw_pressure` events, controller CPU time, contract-map traffic and oscillation (variance of ceilings under a constant load). Workloads: schbench and a request-response server for latency, a parallel build for throughput, a sustained CPU load for thermal saturation, and a mixed interactive plus background load. At least ten randomized repetitions per configuration, with confidence intervals.

## 9. Prior work

| Work | What it already shows |
| --- | --- |
| [Tangram](https://par.nsf.gov/servlets/purl/10164052), MICRO 2019 | Coordinated per-subsystem controllers with safety engines beat cascaded and centralized control |
| [Yukta](https://par.nsf.gov/biblio/10075835), ISCA 2018 | Robust control across hardware and OS layers |
| [SPECTR](https://www.research-collection.ethz.ch/items/0ffb8e18-18d8-452f-8dac-684766ab167d), ASPLOS 2018 | Supervisory control coordinating many-core resource controllers |
| [Hipster](https://upcommons.upc.edu/entities/publication/128ce2f6-6179-43af-9da5-e178ee1545d0), HPCA / TOCS 2017 | Heuristics plus reinforcement learning for core mapping and DVFS on 64-bit ARM big.LITTLE |
| [zTT](https://ee.kaist.ac.kr/ai-in-computer/ztt-learning-based-dvfs-with-zero-thermal-throttling-for-mobile-devices-acm-mobisys-2021-best-paper-award/), MobiSys 2021 | Deep RL for joint CPU and GPU DVFS without thermal throttling |
| [GearDVFS](https://www.cs.cityu.edu.hk/~zhenjili/2023-MobiCom-GearDVFS-Slides.pdf), MobiCom 2023 | Application-agnostic learned DVFS for big, LITTLE and GPU clusters |
| [HiDVFS](https://arxiv.org/abs/2601.06425), 2026 preprint | Hierarchical multi-agent DVFS with a shared reward and a safety shield on Jetson and RubikPi |
| [FUSE](https://arxiv.org/abs/2507.02135), 2025 preprint | On Pixel 7, EAS and the GPU governor, each tuning alone, can drive each other into a downward frequency spiral |
| [ghOSt](https://research.google/pubs/pub50833/), SOSP 2021 | Userspace agents can make Linux scheduling decisions |
| [SchedCP](https://arxiv.org/abs/2509.01245), 2025 | LLM agents generating sched_ext schedulers |
| [scx_lavd](https://github.com/sched-ext/scx/tree/main/scheds/rust/scx_lavd) | Production sched_ext scheduler with core compaction, Energy-Model CPU ordering and `cpuperf` control |

**What may be new**, to be proven by the evaluation rather than claimed: two cooperating controllers built only on upstream sched_ext and power interfaces, coordinated by prices and timescale separation, and evaluated on real ARM64 hardware against EAS, the kernel thermal governor and scx_lavd.
