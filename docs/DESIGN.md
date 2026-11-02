# Lerwatt design

Status: **2026-11-02.** The rule controller, online two-node thermal model,
point forecasts, optional joint MPC, restore journal and Lertick shared-map
contract are implemented. Historical independent-operation and three-cluster
coordination experiments used an eight-vCPU ARM64 VM. Frequencies,
power and temperature were simulated; those records do not establish current
ABI 2 compatibility or real-board behavior.
See [Simulation](SIMULATION.md) for the current measurements and regressions.
The original source study used [Linux v7.3](https://github.com/torvalds/linux/tree/v7.3)
and [sched-ext/scx](https://github.com/sched-ext/scx) as of 2026-10-30.
That is a source reference, not a minimum kernel version or a claim of
compatibility with every driver. Standalone Lerwatt does not require
sched_ext; Lertick integration has separate kernel and BPF requirements.
`deploy.sh` builds an automatically discovering native daemon on the target Linux machine and
provides checks, non-actuating preview, foreground trials and a persistent
systemd service with independent watchdog recovery and learned-parameter
checkpoints. Ordinary installation selects its full BPF-capable build and persists
optional coordination. It never starts QEMU or requires simulator files. See
[continuous operation](SERVICE.md) for installation and validation boundaries.

Labels below distinguish **implemented**, **upstream interface**, and
**planned**. An available Linux interface is not an implemented Lerwatt backend.

## 1. Principle: replace policy, keep mechanism

Linux separates frequency requests, idle-state selection, task placement,
thermal mitigation and device power management. Their decision times differ:
CPUIdle selects a state at idle entry, schedutil reacts to scheduler updates,
and EAS makes placement decisions at task wakeup. A userspace control period
does not replace all of those event-driven decisions.

Here, **replacement** means that the normal decision path delegates a named
policy to the new controller; the original policy may remain an explicit
fallback. A controller that only limits another governor's choices
**constrains** that governor. Current Lerwatt implements the latter for CPU
frequency and idle latency.

| Policy | Linux decision or interface | Current authority |
| --- | --- | --- |
| Frequency request | schedutil and the CPUFreq driver | Lerwatt writes limits, not each frequency request. With compatible schedutil operation, Lertick supplies a `cpuperf` input; schedutil and the driver still process it. |
| Frequency envelope | `scaling_min_freq`, `scaling_max_freq` | Lerwatt never raises the initial maximum. A lower ceiling can lower an existing minimum to preserve min ≤ max. Kernel/firmware constraints can be tighter; other writers are not arbitrated yet. |
| Energy-aware task placement | EAS `find_energy_efficient_cpu()` | Fair-class EAS does not place sched_ext tasks. Lertick uses its existing fixed CPU-selection policy, not learned energy-aware placement. |
| Thermal cooling policy | step_wise, power_allocator and cooling-device bindings | Lerwatt adds CPUFreq constraints; it does not select a thermal governor or take over its cooling-device actions. |
| Power-domain budgets | powercap, RAPL, DTPM | No Lerwatt backend writes these interfaces. An estimated power-to-frequency mapping is not DTPM or hardware power-cap enforcement. |
| Idle-state choice | CPUIdle menu/TEO governors | Lerwatt constrains resume latency through PM QoS. It does not choose a state at each idle entry or write `stateN/disable`. |
| Hardware execution and protection | CPUFreq/CPUIdle drivers, OPP/regulators, firmware, thermal trips | Retained. Lerwatt does not write voltage, disable thermal zones or change trip temperatures. |

Both [menu and TEO](https://docs.kernel.org/admin-guide/pm/cpuidle.html) already
adapt their statistics to observations. Likewise, Linux thermal policies use
feedback. A proposed learned controller must be compared with those adaptive
policies, not described as introducing adaptation to otherwise fixed Linux
algorithms.

## 2. Two controllers, two timescales

| | Lertick (scheduling) | Lerwatt (power) |
| --- | --- | --- |
| Runs in | sched_ext BPF, with a userspace loader | Userspace daemon |
| Timing | Scheduling callbacks; burst-model updates on eligible completed episodes | Default 100 ms loop; optional joint replanning normally held for 120 s, with a raw-temperature override |
| Decides | Fixed placement/dispatch policies; learned bounded slices; utilization-based `cpuperf` targets with optional contract constraints | Per-policy frequency ceilings/floors, contract prices and PM QoS resume-latency constraints |
| Inputs | Task bursts, queue depth, CPU service and wake-latency observations | Thermal-zone readings, `/proc/stat` busy time, `scaling_cur_freq`, discovered power estimates and fresh `lat_crit_milli` |

Lerwatt also reports power from a discovered hwmon `power1_input`, or an
estimate when that is absent. This telemetry is not a measured-energy input
to the current thermal fit; the fit uses estimated per-cluster power.
Frequency residency and the contract's utilization/P99 fields are not current
controller inputs.

Timescale separation, hysteresis and held allocations are engineering measures
intended to reduce oscillation. They do not establish that an inner loop has
settled or prove closed-loop stability. Sensor delay, changing workloads,
CPUFreq rate limits and coupled hardware domains still require evaluation.

## 3. The implemented contract

When its `--lerwatt` option is enabled, Lertick creates and pins two BPF array
maps under `/sys/fs/bpf/lertick/`. They use `BPF_F_MMAPABLE`; Lerwatt opens and
maps them through raw BPF syscalls, without a libbpf dependency. Records have
one designated writer, a sequence counter and an ABI version. This is not a
transaction across clusters or a synchronized global snapshot. Lertick's BPF
reader uses compiler barriers and tolerates a mixed-period copy after field
clamping; the contract must not be treated as a hard safety interlock.

**ABI 2** binds records to the full sorted policy CPU-set hash (`topology_id`)
and a fresh nonzero random Lertick attachment generation (`session_id`). Lerwatt
echoes the session only after fresh compatible demand. A mismatched ABI,
topology or session disables the handoff. The two models, training/checkpoints
and release versions remain private; a model-only update retains the ABI and
does not require a peer update or retraining. See the maintained compatibility
matrix and exact specification in the [shared integration guide](INTEGRATION.md).

**`lw_contract` (written by Lerwatt, read by Lertick per event), one record per cluster:**

| Field | Meaning |
| --- | --- |
| `seq` | Even when stable, odd while being written |
| `version` | Shared record-layout version |
| `topology_id`, `session_id` | Policy CPU-set identity and current Lertick attachment generation |
| `ceiling_cap` | Nonzero upper bound on Lertick's target, on the 0–1024 capacity scale; zero means no contract ceiling |
| `floor_cap` | Lowest capacity to request while latency-critical work runs |
| `energy_price_milli` | Normalized price, 0–1000; currently a fixed ramp as temperature approaches the soft limit |
| `thermal_headroom_mc` | Soft limit minus observed zone temperature, in millidegrees C |
| `valid_until_ns` | `CLOCK_MONOTONIC` expiry, at most 300 ms after publication |

**`lt_demand` (written by Lertick, read by Lerwatt each period), one record per cluster:**

| Field | Meaning |
| --- | --- |
| `seq` | As above |
| `version` | Shared record-layout version |
| `topology_id`, `session_id` | As above; Lerwatt validates before accepting demand |
| `pred_util` | Smoothed per-CPU utilization, averaged by cluster; publication targets 100 ms |
| `lat_crit_milli` | Share of sched_ext CPU runtime classified as latency-critical, 0–1000 |
| `cpuperf_max`, `cpuperf_mean` | Maximum and mean targets published for the cluster |
| `p99_wake_ns` | Upper edge of the log2 histogram bucket containing the wake-latency P99; not an application SLO measurement |
| `updated_ns` | Monotonic-clock freshness timestamp |

Demand publication targets 100 ms; accepted age is at most 500 ms. Absent maps
are retried every second and a silent attachment is dropped after two seconds.
The enabled service requires a 10–100 ms period to fit its 300 ms constraints.
There are at most eight policy entries; records are 48 bytes (`lw_contract`)
and 64 bytes (`lt_demand`). Capacity uses 0–1024 and shares/prices use 0–1000.

The current Lerwatt controller consumes only fresh `lat_crit_milli` from the
demand record. The other fields are available for inspection/future control;
they do not currently drive ceilings, idle constraints or an enforced P99
target. The classifier itself is a fixed verified-wakeup/short-sleep/previous-
burst heuristic in Lertick, not a learned latency classifier.

In Lertick's utilization-target mode, price trims the target by at most 20%,
the latency-critical floor is applied, then the nonzero ceiling and CPU
capacity bound it. `--cpuperf max` does not send these requests, even if maps
are attached. A CPUPerf target is an input to a compatible governor, not a
measured frequency or a hardware watt cap. Price-aware placement is not
implemented. See the [canonical record definition](../include/lerwatt/contract.h).

## 4. Algorithms

**Lertick.** The per-task Q16 burst predictor trains inside BPF. Bounded
predictions choose slices; separate utilization tracking, energy prices and
contract limits determine performance targets. Fairness accounting and CPU
selection remain separate from the burst predictor.

**Rule control.** A PI budget uses Energy-Model power estimates and filtered
temperature, with discrete OPP hysteresis and slower recovery than descent.
A raw-temperature guard bypasses that delay when the sensor reaches the
passive trip minus 3 C.

**Learned dynamics and MPC.** `--policy mpc` fits package heating and cooling
with recursive least squares and learns a cluster hot-spot offset. A
deterministic planner uses that model to select ceilings; this is not a
trained action-selection policy or reinforcement learning. Until the model
is ready, rule control continues. `--cross-cluster` searches at most 65,536
OPP combinations, using estimated CPU-cluster power, and normally holds a
joint allocation for 120 seconds. Eligibility requires trained models,
package/cluster readings and nonzero OPP power estimates. Those estimates
can be relative proxies when no Energy Model was discovered; joint activation
does not establish an accurate physical power model.

**Point forecasts.** `forecast_c` predicts the sensor temperature five seconds
ahead. The default temporal correction learns from completed future labels
and retains its reference when evidence is weak; `--forecast adaptive` uses
the earlier expert blend. These outputs and their delayed-label accuracy
scores are separate from the conservative planning prediction `pred_c`.
In `nominal` mode, changing the point forecaster does not change control decisions.
The [cooperative predictor](COOPERATIVE_PREDICTION.md) can conservatively
use independently validated five-second point evidence in a matching action
context. Unseen actions and the longer hold retain nominal predictions. The
[six-case runtime confirmation](../results/point-model-observations/runtime/README.md)
validates the real daemon against a simulated board's file ABI, not physical
energy savings or a better MPC policy.

Direct daemon invocation defaults to rule control; fresh managed services
default to independent MPC with rule fallback. Cross-cluster planning remains
opt-in. Cooperative prediction is selected independently from automatic optional
Lertick discovery; new installs keep nominal MPC. Plain upgrades
preserve saved choices. This selection is independent of peer connectivity and
does not imply that Lertick improves predictions or that every correction runs.
Contract discovery is enabled by default but is optional, and Lerwatt runs
alone when maps are absent. The evaluation objective is energy-delay product
subject to latency and thermal limits; the current daemon does not measure
or enforce an application P99 constraint.

## 5. Safety envelope and fallback

1. Implemented actuation is limited to `scaling_min_freq`, `scaling_max_freq`,
   per-CPU `power/pm_qos_resume_latency_us` and the contract map. EPP, individual
   idle-state disable, fan/cooling-device state and powercap writes are not
   implemented. CPUFreq targets are clamped to discovered hardware bounds and
   never exceed the initial maximum. A lower ceiling can temporarily lower
   an existing minimum; a frequency bound is not a performance SLO.
2. The journal persists the original value before a knob's first write.
   Failed persistence rolls back the in-memory entry; actuation stops at its
   first error. Restoration validates the full journal before writing and
   retains it on failure. Ceilings are restored before floors to avoid
   applying an original minimum against a still-lowered cap.
   Private journal opens reject symlinks, hardlinks and nonregular files
   without blocking on FIFOs. An unrecovered startup journal prevents new control.
3. Active daemon, manual restore and normal install/remove operations share
   a nonblocking lifetime lock, retained through maintenance file changes.
   Normal expiry and SIGINT/SIGTERM/SIGHUP attempt restoration. The
   foreground deployment helper also waits for the child and attempts restore
   after abnormal exit. In service mode, PID1 monitors successful-loop
   notifications, terminates a stalled worker, runs post-exit restoration and
   restarts it. Foreground processes killed together can still need manual
   `--restore`. The service itself runs with no duration limit and is enabled
   at boot until stopped or uninstalled.
   Per-CPU PM QoS and CPUFreq settings remain shared with other programs;
   external writers and policy changes are not arbitrated by this lock.
4. `--check`, `--describe` and `--dry-run` do not restore journals or create
   runtime state. Dry-run also disables contract publication. Explicit
   telemetry output can still create a new private log file. Existing files
   and special files are rejected; stdout remains an explicit stream. The deployment
   helper fixes the real-machine root. Foreground trials disable scheduler
   coordination; persistent installation can explicitly enable it.
5. Expired contract records give neutral scheduler constraints; stale demand
   stops influencing Lerwatt. This is availability fallback, not a fail-closed
   thermal cap. Kernel passive/critical trips and hardware protections remain
   independent. On the RK3588 reference profile the passive/critical trips are
   85/115 C, with Lerwatt's default soft limit below the passive trip.
6. Service mode detects suspend-clock changes, excessive observation gaps and
   online-CPU changes. It restores and exits so systemd can rediscover inputs
   on restart. Startup checks reject known active competing policy services;
   this is not complete arbitration of later or arbitrary privileged writers.
7. Optional versioned checkpoints retain validated thermal-model parameters
   and long-term gains. Hardware/configuration identity and age are checked.
   Controller bounds, integral state, recent peaks, timing and unfinished
   point forecasts are not restored. Missing/bad checkpoints use fresh
   learning; checkpoint write failure does not disable baseline control.

The raw-temperature guard, bounds and tested recovery paths reduce risks;
they are not a proof of production safety or stability under arbitrary model,
sensor, hardware or control failures.

## 6. Interfaces and gaps

The current HAL reads CPUFreq/CPUIdle sysfs, thermal zones, recognized Intel
coretemp package/AMD Tdie hwmon temperatures, `/proc/stat`, Energy Model
debugfs and the first discovered hwmon `power1_input`. It does
not implement thermal/EM netlink, RAPL energy counters or general power-domain
discovery. Complete EM tables require identified physical units: legacy
milliwatts are converted, and version-dependent flags are not interpreted
without checking their meaning for that kernel. Missing or unknown/abstract
EM data uses a cubic relative-power proxy; an arbitrary
hwmon power channel is not automatically a calibrated whole-board meter.

CPUFreq policy domains need not match thermal or hardware power domains.
Current zone assignment follows cpufreq cooling-device names, falling back
only to a selected package input. Single-package Intel/AMD inputs are
recognized; ambiguous inputs require an explicit `--temperature-sensor`
selection. Known multi-socket systems are rejected even with an override.
Invalid/disabled trip values and hwmon faulted readings are not used.
An unrelated hottest zone is not used as CPU temperature. Driver,
governor, sensor paths and synthetic frequency candidates are reported by
discovery. A capability check validates inputs/access without writing values;
it does not prove hardware measurement accuracy or thermal behavior.
`scaling_cur_freq` can be a last requested P-state rather than instantaneous
hardware frequency. Intel HWP and AMD CPPC autonomous modes select performance
in hardware within OS hints/limits; a CPUFreq knob is not control over that
internal algorithm. See [CPUFreq](https://docs.kernel.org/admin-guide/pm/cpufreq.html),
[intel_pstate](https://docs.kernel.org/admin-guide/pm/intel_pstate.html) and
[amd_pstate](https://www.kernel.org/doc/html/latest/admin-guide/pm/amd-pstate.html).

Fixed implementation capacities are 256 CPU slots, 64 policies, 32 frequency
candidates per policy, 16 zones and eight idle states. CPU/policy bounds,
duplicate IDs and invalid frequency data are rejected. Missing tables use
intermediate limit requests within the CPUFreq range, rather than just two
endpoints that could prevent useful capping. These requests are not physical
OPPs. The Lertick ABI still has eight cluster entries; larger standalone
systems do not silently become coordinated. Service mode stops/restores and
restarts discovery after online-CPU or resume changes. Fully dynamic topology
adaptation and arbitration with other power managers are not implemented.

EAS has its own platform prerequisites; enabling sched_ext does not provide
a learned EAS equivalent. DTPM likewise requires a platform hierarchy and
backends, not just the existence of powercap sysfs. Neither is a dependency
of standalone Lerwatt. See [EAS](https://docs.kernel.org/scheduler/sched-energy.html)
and [DTPM](https://docs.kernel.org/power/powercap/dtpm.html).

## 7. Planned policy expansion and Linux delivery

The table identifies possible decisions to learn, not existing Lerwatt
features. Retain deterministic correctness, hardware constraints, lifecycle
handling and independent safety enforcement in each path.

Thermal response, future workload demand and idle/reuse intervals are useful
prediction targets. A constrained optimizer can allocate budgets using those
predictions; the optimizer and a single-writer coordinator do not themselves
need to be AI. Explicit user choices, sleep inhibitors and fixed operating
profiles can remain rules. No row below asserts that learning is necessary
or already better than the existing policy.

| Policy to investigate | Possible learning target | Retained mechanism / delivery boundary |
| --- | --- | --- |
| CPUFreq request selection | Future CPU demand and performance/energy response | Driver/OPP transitions and limits; optional native governor on compatible drivers |
| CPUIdle per-entry state selection | Non-timer wakeups and useful idle duration | PM QoS, timer/tick rules and driver state entry; custom-kernel governor integration |
| Energy-aware task placement | Per-CPU/domain execution cost and demand | Scheduling correctness and affinity; a Lertick policy extension, not current learned placement |
| Active core count and SMT policy | Benefit of packing work or using more execution contexts | Affinity, CPU hotplug and platform constraints; not a universal core-parking governor |
| Thermal cooling allocation | Thermal response and performance cost of cooling actors | Sensors, trips and cooling-device drivers; new kernel thermal policy or an explicitly delegated userspace policy |
| Fan and platform-profile selection | Temperature/noise response and workload benefit | Supported hwmon/firmware controls and user preferences; some platforms expose profiles instead of direct fan control |
| GPU/interconnect/DRAM frequency policy | Workload sensitivity to each device's performance | Device-specific devfreq/firmware limits; optional native governor where the driver permits replacement |
| Shared power budgets | Marginal benefit of budgets across domains | RAPL/DTPM/powercap enforcement; budgets and time windows remain hardware-dependent |
| Runtime PM and genpd decisions | Reuse intervals, autosuspend timing and useful domain-off residency | Driver suspend/resume sequencing, dependencies and latency constraints; provider/kernel integration for genpd policy |
| Device/link power policy | Workload-dependent value of exposed power states | Storage/network/PCIe protocol and firmware constraints; per-driver support, not a universal PM hook |
| System sleep/wake and charging policy | Optional prediction of user/workload demand | User intent, wakeup correctness and charger/battery protection; separate platform-specific work |
| Display and peripheral idle policy | Optional prediction of user presence and next use | Desktop/display ownership, brightness choices and device lifecycle; existing rules remain a valid starting point |

CPUIdle needs a new prediction target and labels; the existing thermal model
does not predict the next idle interval. A native policy can use bounded
in-kernel inference with slower model updates, but no such path is shipped.
Modules may share telemetry, deployment infrastructure and thermal constraints,
while learning separate performance, idle or device-response models. Reusing
the current thermal coefficients does not supply those new predictions.

Useful backend references include [runtime PM](https://docs.kernel.org/power/runtime_pm.html),
[devfreq](https://docs.kernel.org/driver-api/devfreq.html),
[interconnect](https://docs.kernel.org/driver-api/interconnect.html),
[platform profiles](https://docs.kernel.org/userspace-api/sysfs-platform_profile.html),
[system sleep](https://docs.kernel.org/admin-guide/pm/sleep-states.html) and
[power supplies](https://docs.kernel.org/power/power_supply_class.html).
Device autosuspend, NVMe APST, PCIe ASPM and system suspend are distinct
decisions; a CPU resume-latency request does not control all of them.

Delivery is incremental:

1. **Native deployment helper (implemented).** `deploy.sh` builds on the
   target and offers checks, preview, foreground trials and service management.
   An explicit BPF-free binary remains available; ordinary installation uses
   the full build and automatic ABI 2 discovery. Default install
   enables/starts its own persistent systemd unit after checking hardware and
   known control conflicts. Other managers are not stopped automatically.
   `install-tools` keeps a service-free path for other init systems;
   `PREFIX`/`DESTDIR` support tools-only staging without host service changes.
2. **Lifecycle guards (implemented), wider delivery (future).** Controller/
   restore locking, validated discovery, explicit sensor selection, strict CLI
   validation, non-actuating dry-run and failure-preserving recovery are in
   place. Linux file fixtures and shell-supervision tests exercise failure
   paths without controlling host hardware. Real PID1 fixture tests additionally
   cover enabled unlimited operation, watchdog restart, sensor-loss recovery
   and stop/disable/restore on uninstall. Distribution packages, additional init
   integrations and continuous 24-hour/seven-day physical-hardware validation
   remain future work. Failed restoration preserves its journal and tools.
3. **Optional CPUFreq/devfreq policy add-ons.** Native loadable governors have
   registration APIs, but require a compatible driver, kernel build/API and
   permitted governor switching. A selected `userspace` governor can also
   delegate target decisions to a daemon; that would be policy delegation,
   not in-kernel inference. Neither route is current Lerwatt functionality.
4. **CPUIdle/thermal/genpd kernel integration.** CPUIdle governors cannot be
   unregistered and are not practical loadable modules; the current thermal
   governor registration is internal boot-time code. Genpd governors are
   selected by domain providers, not a generic runtime add-on registry. These
   paths need an appropriate kernel/provider change or a supported future
   extension. Supply optional kernel packages/patches rather than promising
   that DKMS alone enables all policies.

Primary interface references: [CPUIdle governor lifecycle](https://docs.kernel.org/driver-api/pm/cpuidle.html),
[CPUFreq registration](https://github.com/torvalds/linux/blob/master/drivers/cpufreq/cpufreq.c),
[devfreq registration and immutable-governor checks](https://github.com/torvalds/linux/blob/master/drivers/devfreq/devfreq.c),
[thermal registration](https://github.com/torvalds/linux/blob/master/drivers/thermal/thermal_core.c)
and [genpd policy](https://github.com/torvalds/linux/blob/master/drivers/pmdomain/governor.c).
There is no promised generic BPF PM-governor backend in this delivery plan.
Kernel add-ons need matching build information and supported APIs; DKMS does
not supply missing hooks or a stable kernel ABI. Signing/trust requirements
also depend on the installed kernel's policy. See [external modules](https://docs.kernel.org/kbuild/modules.html)
and [module signing](https://docs.kernel.org/admin-guide/module-signing.html).

Build/CI compatibility, simulated control correctness, physical-device
compatibility and measured energy/latency improvement are separate claims.
Publishing an installable package does not establish the last two.

## 8. Hardware and measurement

- **Primary board:** RK3588 (4× Cortex-A55 with `capacity-dmips-mhz` 530 and 4× Cortex-A76 with 1024 in the upstream device tree, three OPP tables, an Energy Model from `dynamic-power-coefficient`, cluster thermal zones). Linux drives voltage through `cpufreq-dt` and regulators, so the full OPP path is visible.
- **Second platform (later):** a Qualcomm board with hardware DVFS and LMh throttling, to test firmware-owned voltage and `hw_pressure`.
- **Energy:** an external meter on the board's input rail, sampled at 1 kHz or faster and time-aligned with traces. The Energy Model is an estimate, not a measurement.
- **Thermal:** fixed ambient, fixed heatsink and fan setting, cool-down to a fixed start temperature before each run.

## 9. Evaluation plan

| # | Scheduler | Power policy |
| --- | --- | --- |
| 1 | EEVDF | schedutil + EAS where supported + kernel thermal governor |
| 2 | Lertick | Kernel policies (schedutil reading Lertick's `cpuperf`) |
| 3 | EEVDF | Lerwatt (ceilings, idle limits, budgets) |
| 4 | Lertick | Lerwatt, contract disabled (no shared prices or latency-critical classification) |
| 5 | Lertick | Lerwatt, contract enabled |

Disabling the contract does not disable Lerwatt's thermal learning or point
forecasts. Hold rule/MPC/point-forecast settings constant when isolating the
contract's effect. scx_lavd in its powersave and autopilot modes is an
additional baseline because it combines core compaction with `cpuperf` control.

Metrics: throughput, p95/p99 latency, energy, energy-delay product, frequency transitions (`power:cpu_frequency` tracepoint), time above the passive trip, `hw_pressure` events, controller CPU time, contract-map traffic and oscillation (variance of ceilings under a constant load). Workloads: schbench and a request-response server for latency, a parallel build for throughput, a sustained CPU load for thermal saturation, and a mixed interactive plus background load. At least ten randomized repetitions per configuration, with confidence intervals.

## 10. Prior work

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
