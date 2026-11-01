# Lerwatt — A Self-Learning Power Manager for Linux

Lerwatt controls CPU frequency ceilings and idle-latency constraints through standard Linux interfaces. It learns a thermal model online, reports temperature forecasts, and can coordinate with the optional [Lertick](https://github.com/Denny-Lin/lertick) scheduler through shared BPF maps.

[![CI](https://github.com/Denny-Lin/lerwatt/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Denny-Lin/lerwatt/actions)
[Simulation and results](docs/SIMULATION.md) · [Design](docs/DESIGN.md) · [Naming](docs/NAMING.md)

## What it does

- Controls cluster frequency floors and ceilings using cpufreq, and idle latency using PM QoS.
- Uses a rule controller by default, with an optional learned model-predictive controller (`--policy mpc`).
- With `--policy mpc`, learns package heating, cooling and cluster hot-spot response while running; no offline training file is required.
- Exposes a five-second temperature point forecast separately from the planner's thermal prediction.
- Can search for a joint frequency allocation across clusters with `--policy mpc --cross-cluster`.
- Saves original settings before changing them and restores them on exit or with `--restore` after a crash.
- Runs independently of Lertick. With Lertick's `--lerwatt` flag, exchanges demand, ceilings and energy prices through real BPF maps.

The first supported simulation profile is an ARM64 RK3588 with four A55 cores and two pairs of A76 cores. The controller itself uses Linux interfaces rather than platform-specific register access.

## Build

Use Linux with a C11 compiler and Make:

```sh
git clone https://github.com/Denny-Lin/lerwatt.git
cd lerwatt
make
make test
make e2e
```

`lerwattd` and `lerwatt-sim` require Linux. The model, forecast, controller and closed-loop tests also run on macOS. No simulator object is linked into the daemon.

## Try the simulated board

In one Linux terminal, start the board. Eight vCPUs are needed to exercise all simulated CPUs with the default mapping:

```sh
sudo build/lerwatt-sim --load procstat --cooling passive
```

In another terminal, inspect it and start the controller:

```sh
sudo build/lerwattd --describe
sudo build/lerwattd --telemetry lerwatt.jsonl
```

The daemon discovers real cpufreq hardware first, then a running simulator at `$LERWATT_SIM_ROOT` or `/run/lerwatt-sim`. Use `--sysroot DIR` to select an explicit simulator tree. Stop the daemon with Ctrl-C before stopping the simulator so it can restore the knobs. A stopped simulator can restart at the same path without `--force`.

To use learned predictions, replace the daemon command with:

```sh
sudo build/lerwattd --policy mpc --telemetry lerwatt-mpc.jsonl
```

Optional joint planning:

```sh
sudo build/lerwattd --policy mpc --cross-cluster --telemetry lerwatt-joint.jsonl
```

MPC uses the rule controller while its model trains. It needs at least about 90 seconds of suitable observations; a hot start or insufficient load variation can take longer. Joint search requires trained models, package and cluster sensors, Energy Models and a bounded number of OPP combinations; otherwise independent control continues.

`forecast_c` is an expected temperature five seconds ahead, separate from the planner's `pred_c`. Its temporal correction learns only from completed forecasts and retains the reference when evidence is weak. `--forecast adaptive` selects the previous expert blend. Both point forecasters leave control decisions unchanged; `-1` means unavailable. See [accuracy, fallback and telemetry](results/point-model-observations/README.md).

On a real board, use the same daemon commands without starting the simulator. No real-board results have been recorded yet.

## Use with Lertick

Build and start [Lertick](https://github.com/Denny-Lin/lertick) on the same Linux machine:

```sh
sudo build/scx_lertick --lerwatt --track-all --duration 120
```

Both controllers may start independently. The daemon retries when the contract is absent; Lertick ignores expired records. Three-cluster operation and standalone scheduler operation have been verified in an eight-vCPU ARM64 VM. See the [VM validation](https://github.com/Denny-Lin/lertick/blob/main/docs/LERWATT_VM_VALIDATION.md).

## Results and limits

The rule, independent MPC and joint MPC paths are tested at the same temperature settings. A shared raw-temperature guard reacts before filtered control, including during startup. All three keep the kernel's own passive and critical trips enabled.

**The rule controller remains the default.** Joint MPC does not consistently improve both work served and frequency-ceiling stability. Forecast accuracy is workload-dependent. Delayed feedback reduces average hot-spot error on reserved simulator workloads, with small regressions in some cases; it does not universally beat persistence. See the [forecast evaluation](results/forecast-feedback/README.md). Full comparisons, regressions and reproduction commands are in [Simulation](docs/SIMULATION.md).

The ARM64 VM runs real Linux scheduling, daemon code and BPF maps. Frequencies, power and thermals are simulated; VM task latency does not represent the simulated board. These results do not establish hardware energy savings or production safety. Real-hardware calibration and measurement are still required.

## Recovery

After a controller crash, restore the journaled values against the same board or simulator root:

```sh
sudo build/lerwattd --restore
```

Lerwatt changes reversible cpufreq and PM QoS settings. It never writes voltages, raises thermal trip temperatures or disables thermal zones. If Lertick stops, Linux restores its default scheduler independently.

## License

Code and documents use the [MIT License](LICENSE).
