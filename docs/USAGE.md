# Usage and compatibility

[← Back to Lerwatt](../README.md)

Detailed requirements, installation, control modes and recovery for Lerwatt. Run the commands below from the Lerwatt checkout root unless a section specifies another checkout.

## Compatibility

The deployment helper builds on the target machine and checks actual Linux interfaces instead of guessing from a distribution or kernel version number. Default installation enables and starts a persistent systemd service. Foreground trials and `install-tools` remain available without systemd. See [continuous operation and watchdog recovery](SERVICE.md).

| Component | Current requirement or behavior |
| --- | --- |
| Operating system | Linux with a C11 compiler, Make, libc/libm and Linux userspace API headers. CI targets Ubuntu 22.04 x86_64, Ubuntu 24.04 ARM64/x86_64 and Alpine/musl deployment fixtures; this is separate from physical-hardware measurements. |
| CPU control | Usable CPUFreq policies and valid frequency information. Active control needs permission to write their `scaling_min_freq` and `scaling_max_freq` files, normally root. |
| Thermal inputs | CPU thermal zones, Intel `x86_pkg_temp`/`coretemp` package sensors, or AMD `k10temp` Tdie. An explicit `--temperature-sensor /sys/...` can select another CPU input in millidegrees C. Missing/ambiguous CPU inputs block the trial. |
| Idle constraints | Per-CPU `power/pm_qos_resume_latency_us` is optional; absent files are skipped. Per-entry idle-state selection stays with the kernel governor. |
| Power estimates | A complete Linux Energy Model with identified physical units is used when available. Legacy milliwatts are converted to microwatts; unknown/abstract scales use a cubic relative-power proxy whose values are not calibrated watts. A discovered hwmon power channel is logged as reported by that device, not automatically as whole-machine power. |
| Lertick | Optional. New independent installations and hardware trials use the BPF-free build. `./deploy.sh install --lertick` selects the full build and persistent coordination, requiring usable Linux userspace BPF headers and the separate [Lertick kernel/toolchain requirements](https://github.com/Denny-Lin/lertick). See the [shared integration guide](INTEGRATION.md). |

Deployment environments actually tested on **2026-11-01 (America/Los_Angeles)**:

| Ubuntu version | Architecture and environment | Checks completed |
| --- | --- | --- |
| 22.04.5 LTS | ARM64 and emulated x86_64 containers; GCC 11.4, glibc 2.35 | Build, deployment/recovery fixtures, ordinary-user sudo tools-only install, restore and uninstall |
| 24.04.4 LTS | ARM64, isolated container; GCC 13.3, glibc 2.39 | Native build, deployment/recovery fixtures, ordinary-user sudo tools-only install, restore and uninstall |
| 24.04.5 LTS | Emulated x86_64 container; GCC 13.3, glibc 2.39 | Build, deployment/recovery fixtures, ordinary-user sudo tools-only install, restore and uninstall |
| 24.04.5 LTS | ARM64 VM, Linux 7.0.0-38-generic; GCC 13.3, glibc 2.39 | Native build, deployment/recovery fixtures, model/control tests and simulator end-to-end recovery |

These are software and installation checks with temporary control fixtures. Container tests share the container host's kernel; they do not validate Ubuntu's stock kernel or physical CPU, sensors, energy savings or battery life. Emulated x86_64 execution checks userspace behavior, not physical x86 hardware. The CI matrix above describes configured jobs; the local results here are reported separately. Ubuntu versions not listed here have not been verified in this deployment pass.

Discovery supports CPU IDs 0–255, up to 64 CPUFreq policies, 32 frequency entries per policy, 16 thermal zones and reads up to eight idle states. CPU/policy bounds and invalid frequency data are rejected before control. For drivers without frequency tables, including common Intel P-state configurations, the HAL generates bounded intermediate frequency-limit candidates; these are control requests, not measured hardware OPPs.

Automatic package-sensor selection targets single-package systems. Ambiguous sensor inputs on a single-package system require an explicit choice that represents all controlled policies. Known multi-socket systems are rejected even with an override, because per-package sensor mapping is not implemented. AMD Tctl is not automatically treated as a physical die temperature. Inspect the reported sensor paths before a trial. Invalid/disabled thermal trips are ignored, and a sensor reporting a hwmon fault blocks or stops control.

Some AMD CPUs expose Tctl without Tdie, so automatic selection may fail even with a working k10temp driver; choose an appropriate sensor and limits explicitly for that CPU rather than assuming a generic physical-temperature interpretation. See [k10temp](https://docs.kernel.org/hwmon/k10temp.html). Every CPU in a discovered policy must currently have a `/proc/stat` record; policies containing offline CPUs are rejected by the capability check. More than 16 discovered thermal zones is also unsupported, including unrelated zones. VM guests, WSL and ordinary containers often lack writable CPUFreq or usable CPU sensors and are not generally hardware-control targets.

On hardware-managed P-state systems, frequency limits constrain the driver/hardware policy; they do not prescribe the instantaneous physical frequency. See the Linux [CPUFreq](https://docs.kernel.org/admin-guide/pm/cpufreq.html), [Intel](https://docs.kernel.org/admin-guide/pm/intel_pstate.html) and [AMD](https://docs.kernel.org/admin-guide/pm/amd-pstate.html) documentation.

## Deploy on your own Linux hardware

Install your distribution's C compiler, Make and development headers first. For example, Debian/Ubuntu provides `build-essential`; Alpine provides `build-base` and `linux-headers`. The script reports missing build requirements rather than installing packages automatically. Python 3 is only needed for development tests, not deployment.

On Ubuntu 22.04/24.04, install the build tools and Git first:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends git build-essential ca-certificates
```

```sh
git clone https://github.com/Denny-Lin/lerwatt.git
cd lerwatt
./deploy.sh check
./deploy.sh install
lerwatt status
lerwatt logs
```

Run these commands as your normal user. The helper builds `build/lerwattd-standalone` locally and requests sudo only for hardware checks/control or system installation. If already root, build first with `make hardware`. No model download or offline training is needed.

| Command | Action |
| --- | --- |
| `./deploy.sh check` | Report CPUFreq driver/governor, frequency choices, sensor paths, model inputs and access to control files; write no settings. |
| `./deploy.sh preview` | Learn and log without changing CPU limits or publishing scheduler control. |
| `./deploy.sh run` | Check first, then run MPC for 300 seconds by default and restore when the child exits. Ctrl-C stops the trial. |
| `./deploy.sh restore` | Restore the real-machine recovery journal; refuse if another controller holds the lock. |
| `./deploy.sh install` | Install the tools and systemd unit, enable at boot and start continuous operation after checks pass. |
| `./deploy.sh install-tools` | Install tools only; no service activation. Useful with other init systems. |
| `lerwatt status`, `lerwatt logs` | Inspect the managed service and recent journal messages. |
| `lerwatt start`, `lerwatt stop`, `lerwatt restart` | Manage continuous control; stopping restores settings. |
| `./deploy.sh uninstall` | Stop/disable the owned service, restore before removal, and keep recovery tools/data on failure. |

To install the commands for use outside the checkout:

```sh
./deploy.sh install
lerwatt status
lerwatt logs
```

**No reboot is required to install or start.** This installs a userspace daemon and helper using existing CPUFreq/PM QoS interfaces. Default installation starts continuous control and enables the service at boot. systemd independently supervises the worker and restarts after failures; stopping or uninstalling attempts restoration. `install-tools` opts out of service changes. A failed hardware or control-ownership check prevents activation. [Persistent service details](SERVICE.md).

`PREFIX` changes the install location; `DESTDIR` supports files-only staging without running the daemon, sudo or systemctl. `make install` and `make uninstall` call the same helper. No dependencies are downloaded, governors changed or other power managers stopped automatically. Default installation changes boot-time behavior by enabling its own unit.

System installation requires a root-owned destination and ancestors with no group/other write permission or symbolic-link components. Use the default `/usr/local`, or another protected prefix such as `/opt/lerwatt`. Package staging should run as a normal user. Run privileged commands only from a trusted checkout; installation does not authenticate downloaded source code.

## Run on your Linux machine

The helper is the simplest path. The equivalent direct-daemon commands below are useful for advanced trials. A shared lock serializes active controllers and restoration. External programs are not part of that lock: check ownership of overlapping knobs when using TLP, auto-cpufreq, TuneD, power-profiles-daemon or thermald. Do not indiscriminately disable thermal protection services.

### 1. Inspect the machine

```sh
sudo build/lerwattd-standalone --sysroot / --check
```

Confirm the hardware root, policies, frequency choices and CPU/package sensor paths. A successful check establishes discovered temperature/frequency inputs and CPUFreq file access; runtime sampling and optional QoS access can still fail and trigger restoration. For a CPU sensor that is not automatically recognized, select its actual path, for example:

```sh
./deploy.sh check --temperature-sensor /sys/class/hwmon/hwmon3/temp2_input
```

This is an example path: use your CPU sensor, not an unrelated device. Pass the same option to preview and run. Without a mapped package input, the daemon reports rule fallback for thermal planning. When no passive trip is available, the default soft limit is 80°C; use `--soft-limit` to choose an appropriate lower limit for your hardware.

### 2. Preview without affecting CPU limits or Lertick

```sh
sudo build/lerwattd-standalone --sysroot / --policy mpc --dry-run --no-contract \
  --duration 180 --telemetry lerwatt-preview.jsonl
```

Dry-run does not write control knobs, publish contract records, restore old settings or create runtime state. With valid inputs, it can learn in memory and write requested telemetry. The helper also fixes `--no-contract` explicitly for hardware trials.

### 3. Try learned control in the foreground

Before service installation, or after `lerwatt stop`, run a finite standalone trial. An active service holds the shared controller lock; do not run a second active controller alongside it. Start the managed service again with `lerwatt start` after the trial.

```sh
sudo build/lerwattd-standalone --sysroot / --policy mpc --no-contract \
  --duration 300 --telemetry lerwatt-mpc.jsonl
```

The daemon exits after five minutes; Ctrl-C stops it earlier. Normal exit, SIGINT and SIGTERM attempt to restore journaled settings. Check the final write-error and restore messages. Use `--policy rule` for the default PI controller instead.

`--sysroot /` explicitly selects this machine. Without an explicit root, discovery prefers real CPUFreq hardware and otherwise looks for a simulator; the examples always select a root to keep the two uses distinct.

## Learning and control modes

| Mode or output | What it does |
| --- | --- |
| `--policy rule` (default) | PI thermal-budget control, filtering and discrete frequency-ceiling hysteresis. |
| `--policy mpc` | Learns thermal dynamics and uses conservative planning predictions to choose frequency ceilings; rule fallback until ready or when required inputs are absent. |
| `--policy mpc --cross-cluster` | Searches a bounded combination of frequency choices across clusters using their thermal models and estimated power. |
| `--forecast temporal` (MPC default) | Reports a five-second point forecast with a delayed-label temperature-history correction. |
| `--forecast adaptive` | Reports the previous expert-blend point forecast for comparison. |
| `--prediction nominal` (default) | Keeps the original thermal MPC and point-forecast separation. |
| `--prediction cooperative` | Uses fresh, action-supported five-second point evidence to tighten MPC predictions. Requires MPC, temporal forecasts and a 10–100 ms period; longer/unseen actions retain the nominal model. |

The thermal model needs at least about 90 seconds of suitable observations; a hot start or insufficient load variation can take longer. Raw launches still start fresh unless `--model-state` is selected. The managed service saves and reloads validated long-term thermal parameters; transient control, recent peaks, load history and pending point forecasts start fresh. Checkpoints have hardware/configuration fingerprints, a format version and a 30-day age bound. [Checkpoint behavior](SERVICE.md#learned-thermal-state).

Joint planning requires trained models, valid temperature inputs, nonzero power estimates and at most 65,536 frequency combinations. Otherwise independent control continues. Its power inputs can come from the Energy Model or the relative proxy; proxy-based planning does not establish a physical-watt optimum.

Telemetry separates `pred_c` (planning prediction) from `forecast_c` (expected temperature five seconds ahead). In default `nominal` mode, the point forecast does not influence ceilings. Opt-in `cooperative` mode can tighten five-second MPC decisions within its supported action domain, while preserving the original raw guard and thermal limits. [Cooperative prediction and telemetry](COOPERATIVE_PREDICTION.md). `model` and `joint` indicate thermal-model readiness and joint-planner activation; `-1` indicates an unavailable temperature prediction. See [forecast telemetry and fallback](../results/point-model-observations/README.md#runtime-and-telemetry).

`--telemetry FILE` creates a **new** file with mode `0600`; existing files, symlinks, hardlinks, FIFOs and devices are rejected instead of appended to. Choose a fresh filename for each run. `--telemetry -` writes to stdout, so ordinary shell redirection can create a user-owned log. Use an output directory you trust; log size is not rotated or capped automatically.

Frequency floors, idle-latency requests and energy prices currently use rules. The shared raw-temperature guard is independent of the learned model. Lerwatt does not write voltages, change thermal trips or disable thermal zones; existing driver and hardware protections remain responsible for enforcement.

## Stop, restore and uninstall

For continuous operation, `lerwatt stop` stops the unit and restores controls; `lerwatt start` resumes it. `lerwatt uninstall` additionally disables automatic startup and removes the managed unit/tools. For a foreground trial, use Ctrl-C or let `--duration` expire; its helper waits for the child and attempts recovery, including abnormal exits.

After an abnormal exit, restore against the **same root** used by that run. For hardware:

```sh
./deploy.sh restore
```

After stopping an installed service, `lerwatt restore` is equivalent. Restoration refuses an active controller's lock. Advanced simulator users must use the full daemon with that simulator's exact `--sysroot` and keep its ABI files available until restoration succeeds; the hardware helper always selects `/`.

If both supervisor and daemon are killed, use manual restoration. A subsequent active launch also attempts recovery and refuses control when recovery fails. The journal is `/run/lerwatt/restore.journal` on hardware; the instance-lock file is intentionally retained. Lertick contract expiry does not undo sysfs limits.

To remove an installation after stopping the trial:

```sh
lerwatt uninstall
```

You can also use `./deploy.sh uninstall` from the checkout with the same `PREFIX`. The unit is stopped/disabled first; a recovery failure then prevents removal of the tools and unit so repair remains possible. Configuration, model checkpoints, telemetry and recovery directories are retained.

The controller, restoration and normal install/remove operations share one lock; maintenance keeps it through the file changes. Recovery validates the whole journal before writing and restores frequency ceilings before floors. Keep the journal when recovery reports an error.

For initial hardware trials, remain at the machine and use the finite default duration. Service mode detects suspend/resume gaps and online-CPU changes, restores and exits so systemd can restart with fresh discovery. Arbitrary hotplug layouts may still fail recovery/checks and leave the service retrying safely. Known policy-service conflicts are checked at startup; later competing writers are not fully arbitrated. A sensor that stays numeric but stops updating cannot reliably be detected. Foreground processes killed together can require manual recovery; the managed service has PID1 watchdog and post-exit restoration. Kernel/firmware protection must stay enabled. Physical-board safety, performance and continuous 24-hour/seven-day operation remain unverified.

## Try the simulated board

This is a developer-only path; hardware deployment above does not need it. Build the simulator and full daemon with `make all` first. The RK3588 profile models four A55 cores and two pairs of A76 cores. Eight Linux vCPUs are needed to exercise all simulated CPUs with the default mapping; use `--cpu-map` for a smaller VM.

In one terminal, start an isolated simulator:

```sh
sudo build/lerwatt-sim --root /run/lerwatt-sim --load procstat \
  --cooling passive --no-cpuperf
```

In another terminal, explicitly select that simulator:

```sh
sudo build/lerwattd --sysroot /run/lerwatt-sim --describe
sudo build/lerwattd --sysroot /run/lerwatt-sim --policy mpc --no-contract \
  --duration 300 --telemetry lerwatt-sim.jsonl
```

Stop the daemon and verify restoration before stopping the simulator. A stopped simulator can restart at the same path without `--force`. Do not omit `--sysroot` in this demo: real CPUFreq hardware takes precedence during auto-detection, even when a simulator is running.

The VM executes the real daemon, Linux scheduling and file operations, while board frequency, power and temperature are simulated. VM task latency does not represent the simulated board. See [simulation fidelity and reproduction](SIMULATION.md).

## Optional Lertick integration

For persistent native use, run this from the Lerwatt checkout:

```sh
./deploy.sh install --lertick --period-ms 100
```

This builds the full daemon without libbpf, persists `contract=auto` and keeps Lerwatt's watchdog/recovery. Install Lertick separately, then use `lertick integration on` and `lertick enable`. Lertick installation alone does not enable its service. Plain upgrades preserve modes; `./deploy.sh install --no-lertick` and `lertick integration off` turn the link off.

Either controller may start first. ABI 2 checks policy topology, attachment identity and freshness; unavailable or incompatible peers leave independent operation. Models remain private and compatible model changes do not require joint retraining. The [shared integration guide](INTEGRATION.md) covers installation, status, independent upgrades and exact protocol rules.

For raw-loader research, build with `make hardware-lertick`, select the appropriate explicit `--sysroot`, and omit `--no-contract` to request the link. A coordinated simulator experiment also needs the simulator's CPUperf behavior enabled; use the [historical VM validation setup](https://github.com/Denny-Lin/lertick/blob/main/docs/LERWATT_VM_VALIDATION.md) as background, and revalidate its old protocol observations for the current ABI. A hardware service never depends on that simulator.

## Current scope and expansion

Lerwatt currently controls CPU frequency floors/ceilings and CPU idle-latency constraints. The existing CPUFreq, CPUIdle and thermal governor implementations remain in place. It does not yet manage GPU frequency, device autosuspend, fans, charging or system sleep.

| Area | Current status and expansion direction |
| --- | --- |
| Thermal learning and CPU ceilings | Implemented, opt-in MPC; point-forecast accuracy is evaluated separately. |
| Demand-aware CPU frequency and per-entry idle selection | Future learning targets: performance demand and idle duration. A thermal model alone does not supply these predictions. |
| GPU/DDR/uncore DVFS, device/domain idle and shared budgets | Future hardware-specific policies and adapters; availability depends on kernel drivers and firmware. |
| Fan, storage/network power states, battery, display and system sleep | Future policies with device-specific interfaces and user-intent constraints. |
| Limits, driver transitions, dependencies and recovery | Keep deterministic. Learning proposes decisions; correctness, protection and restoration do not depend on a model. |

No power-management function inherently requires AI. Learning is useful where predictions can improve a policy; existing statistical/adaptive governors remain meaningful baselines. See the [complete expansion map and delivery boundaries](DESIGN.md).

### Further deployment work

The native helper, capability checks, bounded discovery, controller/restore lock and failure-preserving recovery are implemented. Further work includes:

- Broader hardware-owner reports and per-package sensor mapping for multi-socket systems.
- Optional distribution packages and support for additional init systems; systemd service integration is implemented.
- Broader hotplug/resume validation and full arbitration with other power-management services.

Kernel-policy replacements remain a separate extension path: installing the current daemon does not by itself replace a governor's normal decision path.

## Development and reproduction

```sh
make test
make e2e
make test-deployment
```

The pure model, forecast, controller and closed-loop tests also run on macOS. Linux is required for daemon/simulator integration. No simulator object is linked into the daemon.

The [six-case runtime reproduction](../results/point-model-observations/runtime/README.md#reproduction) takes roughly two hours plus scoring and uses fresh isolated simulator roots. It is a validation procedure, not an installation requirement.

## License

Code and documents use the [MIT License](../LICENSE). See also [Naming](NAMING.md).
