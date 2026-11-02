<h1 align="center">Lerwatt</h1>

<p align="center">AI-powered CPU power management. Built for your Linux.</p>

<p align="center">
  <a href="#get-started-on-ubuntu">Get started →</a> &nbsp;&nbsp;
  <a href="#how-it-works">Explore Lerwatt →</a> &nbsp;&nbsp;
  <a href="docs/USAGE.md">Read the guide →</a>
</p>

<p align="center">
  <img src="docs/assets/lerwatt-overview.svg" alt="A little foresight for your Linux CPU. Lerwatt learns thermal behavior locally and plans CPU frequency ceilings, without a model download or reboot." width="1120" />
</p>

<p align="center">
  <strong>Learns your machine.</strong> Builds a thermal model while it runs.<br />
  <strong>Plans ahead.</strong> Uses that model to choose predictive CPU ceilings.<br />
  <strong>Runs around the clock.</strong> Starts at boot. Restarts after failures. Uninstall to remove.
</p>

<p align="center">
  <a href="https://github.com/Denny-Lin/lerwatt/actions"><img src="https://github.com/Denny-Lin/lerwatt/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-86868b?style=flat-square" alt="MIT license" /></a>
  <img src="https://img.shields.io/badge/hardware_trials-experimental-86868b?style=flat-square" alt="Experimental hardware trials" />
</p>

<br />

## Get started on Ubuntu

**Ubuntu 22.04 / 24.04 · systemd · sudo access · no reboot**

Use a normal user account. The helper builds locally and asks for sudo when needed.

**1. Prepare and check**

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends git build-essential ca-certificates
git clone https://github.com/Denny-Lin/lerwatt.git
cd lerwatt
./deploy.sh check
```

**2. Install and start continuous control**

```sh
./deploy.sh install
```

Installation enables `lerwatt.service` at boot and starts it immediately. There is no five-minute limit: the service keeps running, including after reboot, until you stop or uninstall it. The installer reports active competing policy services instead of disabling them automatically.

**3. See how it is doing**

```sh
lerwatt status
lerwatt logs
```

An independent systemd watchdog supervises progress. A stalled worker is stopped, its settings are restored where possible, and the service restarts. Missing sensors pause control through the same recovery path.

```mermaid
flowchart LR
    A[Install + enable] --> B[Run continuously]
    B -->|Failure| C[Stop + restore]
    C --> D[Restart automatically]
    D --> B
    B -->|Uninstall| E[Stop + disable + restore]
    classDef default fill:#f5f5f7,stroke:#d2d2d7,color:#1d1d1f
    linkStyle default stroke:#86868b,stroke-width:1.5px
```

To remove Lerwatt and stop automatic startup:

```sh
lerwatt uninstall
```

Uninstall stops and disables the service, restores journaled settings, then removes the managed tools and unit. Recovery failures preserve the recovery tools and journal for repair.

Want a trial first? `./deploy.sh preview --duration 60` leaves control settings unchanged; `./deploy.sh run --duration 300` runs a finite foreground trial before installing the service. `install-tools` keeps a tools-only option for other init systems. [Service, configuration and recovery →](docs/SERVICE.md)

## How it works

```mermaid
flowchart LR
    S[CPU temperature + load] --> M[Online thermal model]
    M --> P[Plan CPU limits]
    P --> K[Linux CPUFreq]
    K --> C[Your CPU]
    C --> S
    S -.-> F[Five-second point forecast]
    F -.-> T[Telemetry]
    classDef default fill:#f5f5f7,stroke:#d2d2d7,color:#1d1d1f
    classDef learned fill:#edf5ff,stroke:#a8c8ee,color:#1d1d1f
    classDef linux fill:#f5f5f7,stroke:#d2d2d7,color:#1d1d1f
    linkStyle default stroke:#86868b,stroke-width:1.5px
    class M,P learned
    class K,C linux
```

The thermal model learns online; rule control covers warm-up and fallback. The service saves compatible learned thermal parameters every five minutes and on normal shutdown, then reuses them after restart. Fresh learning usually needs at least about 90 seconds of suitable observations. A preview does not pretrain the service; control history and unfinished forecasts are always refreshed.

Linux continues handling frequency transitions, idle-state selection and thermal protection. Learned planning sets CPU frequency ceilings; frequency floors and PM QoS idle-latency constraints currently use rules. The separate five-second point forecast is reported in telemetry; it does not drive control decisions. [Architecture and expansion →](docs/DESIGN.md)

## Works with Lertick

Let [Lertick](https://github.com/Denny-Lin/lertick) handle task scheduling while Lerwatt manages CPU power constraints. Enable their optional local link with `./deploy.sh install --lertick`; each keeps its own model, updates and recovery. Model improvements that keep the protocol compatible do not require updating or retraining the peer. Either project can run alone. **[Connect them in a few steps →](docs/INTEGRATION.md)**

## Tested Ubuntu versions

| Version | Software environments tested | Result |
| --- | --- | --- |
| **22.04.5 LTS** | ARM64 and x86_64 containers | Build, deployment tests and tools-only installation/recovery passed |
| **24.04.4 LTS** | ARM64 container | Tools-only installation and deployment checks passed |
| **24.04.5 LTS** | x86_64 container; ARM64 VM | Tools-only deployment passed; ARM64 also passed model/control and end-to-end tests |

x86_64 execution used emulation. These validate software and installation; physical-board behavior remains to be measured. [Exact environments and compatibility →](docs/USAGE.md#compatibility)

The continuous service has also passed real systemd install, watchdog/restart, sensor-loss recovery and uninstall tests on Ubuntu 24.04.5 ARM64 with temporary CPU-control fixtures. Continuous **24-hour and seven-day physical-hardware runs remain unvalidated**. [Service validation →](docs/SERVICE.md#validation-and-limits)

**Hardware requirements:** usable CPUFreq controls and a CPU temperature input. Known multi-socket systems, offline CPUs in a discovered policy and some sensor/topology layouts are currently unsupported. `lerwatt check` reports missing capabilities. Keep kernel/firmware thermal protection enabled and check for competing power managers before a trial. [Requirements and limitations →](docs/USAGE.md#compatibility)

## Results so far

**Five-second forecast error: 1.47°C → 0.67°C mean absolute error.**

That is a **54.47% reduction** against Lerwatt's previous reference forecaster across six simulated daemon profiles, including fallback observations. A separate 192-run module study reduced mean forecast error by **28.95–29.51%**. The studies use different datasets and are reported separately.

Physical energy savings, battery life and throughput gains remain unmeasured. Predictive control remains experimental; its control trade-offs are evaluated separately from point-forecast accuracy.

**[Daemon results and raw records](results/point-model-observations/runtime/README.md)** · **[Module study](results/point-model-observations/README.md)** · **[Control comparisons](docs/SIMULATION.md#4-control-and-forecast-results)**

## Go further

| I want to… | Start here |
| --- | --- |
| Configure continuous operation or understand the watchdog | [Persistent service](docs/SERVICE.md) |
| Select a sensor, log telemetry or recover a trial | [Usage and recovery](docs/USAGE.md) |
| Understand the AI model and deterministic limits | [Design](docs/DESIGN.md) |
| Connect the optional Lertick scheduler | [Shared integration guide](docs/INTEGRATION.md) |
| Reproduce the experiments or contribute | [Development](docs/USAGE.md#development-and-reproduction) · [Simulator](docs/SIMULATION.md) |
| Share a hardware trial or report a problem | [Open an issue](https://github.com/Denny-Lin/lerwatt/issues) |

Lerwatt is **learning + watt**, pronounced **LER-watt**. [MIT licensed](LICENSE).
