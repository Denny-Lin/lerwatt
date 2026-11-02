# Working on Lerwatt

Keep native Linux deployment usable. Build as the normal user; never build or
download dependencies as root. Preserve journal recovery, single-controller
locking, watchdog progress from successful control cycles, and kernel/firmware
thermal protection. Hardware control and fault injection require an explicitly
authorized test environment; temporary file fixtures are the default.

## Independent AI updates and Lertick

Lerwatt owns CPU power policy, CPUFreq min/max and PM QoS constraints. Lertick
owns task scheduling, slices and CPU performance requests. Neither controller
starts, stops, upgrades, trains or writes the private model state of its peer.
Keep the optional link independent of both service lifecycles.

The public interface is documented in [docs/INTEGRATION.md](docs/INTEGRATION.md).
Model/algorithm/application release versions are independent of contract ABI 2.
An ordinary model improvement must retain field layouts, meanings, units,
identity checks, timing/freshness bounds and control ownership. Do not bump the
ABI merely because a model changes. Inference/training must fit the cadence.

Run `make test-integration-abi` and `make test-deployment` on Linux for changes
affecting integration/deployment. When both checkouts are available, also run
`python3 tests/check_integration_abi.py --peer ../lertick`. Keep the vendored
contract header and shared integration guide synchronized. A deliberate schema
change needs an ABI migration and compatibility matrix, including safe mixed
versions; never silently reinterpret old records or weaken identity/freshness.

For integration lifecycle changes, use the explicit VM-only paired tests and
record the exact distro/architecture/kernel. Prove either startup order,
single-peer restarts, expiry, reconnection and independent operation. Preserve
each project's integration setting on update. Report configured and currently
connected states separately.

Prediction accuracy alone does not establish better power control. Model
improvements need appropriate closed-loop temperature, energy, performance and
overhead evaluation against the previous method and baseline. Preserve adverse
cases and historical evidence; short simulator/VM tests do not establish
physical-machine energy savings or 24-hour/seven-day reliability.
