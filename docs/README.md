# Lerwatt documentation

[← Product overview](../README.md)

## Install and use

| I want to… | Read |
| --- | --- |
| Install, run continuously, update or uninstall | [Service guide](SERVICE.md) |
| Check hardware support, choose a sensor or run a trial | [Usage and compatibility](USAGE.md) |
| Connect Lertick while keeping both AIs independent | [Integration guide](INTEGRATION.md) |

## Understand and develop

| Topic | Read |
| --- | --- |
| Shared, replaceable predictor and conservative MPC integration | [Cooperative prediction](COOPERATIVE_PREDICTION.md) |
| Model, controller, Linux interfaces and expansion boundaries | [Architecture](DESIGN.md) |
| Reproduce experiments and understand the simulated hardware | [Simulation](SIMULATION.md) · [Development commands](USAGE.md#development-and-reproduction) |
| Project name | [Naming](NAMING.md) |

## Results and validation

| Evidence | Scope |
| --- | --- |
| [Controller integration](../results/integration-abi2/README.md) | Independent method changes, real BPF maps and service lifecycle in a VM |
| [Temperature prediction](../results/point-model-observations/README.md) | Module evaluation and recorded limitations |
| [Daemon prediction](../results/point-model-observations/runtime/README.md) | Native daemon against simulated hardware interfaces |
| [Thermal control](../results/thermal-control/README.md) | Closed-loop control comparisons |
| [Forecast feedback](../results/forecast-feedback/README.md) | Earlier forecast experiments |

The [product overview](../README.md) is the entry point; `docs/` contains guides and `results/` retains experimental evidence. Runtime code lives in `src/`, public interfaces in `include/`, and deployment starts with the root `deploy.sh`. [AGENTS.md](../AGENTS.md) records repository instructions for coding agents. The project uses the root [MIT license](../LICENSE).
