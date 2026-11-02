# Installation-specific prediction defaults

Baseline: `d647c97`. This working-tree change alters installation selection,
status output and tests, not the predictor, MPC algorithm, thermal constraints,
checkpoint format or Lertick contract ABI. [Current behavior](../../docs/COOPERATIVE_PREDICTION.md).

## Selection and persistence

| Installation request | Coordination | Prediction |
| --- | --- | --- |
| New `install` | off | nominal, independent MPC |
| Explicit `install --lertick` | auto | cooperative |
| Explicit `install --no-lertick` | off | nominal |
| Either mode with `--prediction` | as selected | explicit value wins in either argument order |
| Plain upgrade | saved setting | saved setting, including nominal opt-outs |
| Plain upgrade of legacy config without prediction key | saved setting | nominal |

Selecting `--lertick` again applies its cooperative default; include
`--prediction nominal` to retain an explicit opt-out. Previously selected rule
or slow-period settings are not silently overwritten: incompatible cooperative
requests fail before stopping the old service. Choose MPC with a 10–100 ms
period or explicitly select nominal. Available binaries, peer connectivity and
scheduler startup never choose the prediction mode.

Enabled does not mean applied. Existing action, horizon, freshness, uncertainty
and delayed-label reliability gates still decide whether to use a correction.
Unsupported/unreliable corrections retain nominal MPC; unavailable/invalid
required models retain rule fallback. Raw guard, journal restoration,
watchdog, model compatibility/version validation and kernel/firmware protection
are unchanged. Models remain privately owned and independently replaceable;
compatible Lerwatt model upgrades do not retrain or restart Lertick. This default
does not imply that Lertick makes Lerwatt forecasts more accurate.

## Validation

- Portable `make test` passed, including learned-correction control influence,
  unsupported/invalid prediction rejection, replacement backend tests and the
  complete finite-job comparison. [Log](portable-tests.log).
- Deployment fixtures passed fresh standalone defaults, explicit integration
  selection, prediction overrides in both orders, standalone opt-in, explicit
  opt-out, plain upgrades, legacy configs, incompatible configurations and
  existing trust/conflict/recovery protections. [Log](deployment-tests.log).
- Linux `make all test-deployment e2e` and `build/test_predict` passed.
  ABI 2's 56 compiled values also match the Lertick source checkout.
  [Linux log](linux-validation.log), [peer ABI check](abi.log).
- A real PID 1 systemd manager passed two fresh installations without an
  explicit prediction override: standalone selected nominal; `--lertick`
  selected cooperative. Both reached unlimited ready operation, recovered from
  stopped-worker watchdog and sensor loss, and restored settings on uninstall.
  Special-character executable prefixes were included. Integration configured
  cooperative while waiting independently for an absent scheduler; this is
  not a connected-pair benchmark. Only unique test units and temporary hardware
  files were controlled, not physical CPU settings. [Log](linux-validation.log).
- The existing synthetic learning test still changes the MPC ceiling from
  2.4 to 1.8 GHz when reliable; the raw guard then forces 1.2 GHz. No learning
  weights or reliability flags are injected into the LMS. This demonstrates
  functional control influence, not an energy or performance benefit.

Linux environment: ARM64 Ubuntu VM, kernel `7.0.0-38-generic`, eight vCPUs.
All 61 source/test files in the VM match the local validation snapshot;
test workers exited, sched_ext remained disabled, and the earlier `/run`
archive was retained. [Identity and environment](linux-source-identity.json).

## Preserved performance regressions

The new [60 rows](finite-jobs.jsonl) reproduce all earlier control/forecast
outcomes, excluding CPU timing. All jobs complete, no backlog or simulated
throttling remains, and peak temperature is at most 82.23 C. Twenty-eight of
30 pairs have identical outcomes. Ambient-ramp independent-MPC seeds 7 and 99
still increase makespan by 0.02 and 0.07 s respectively, with small forecast
and modeled-energy differences. The strict nonregression gate still fails
on those two pairs: [summary](control-summary.json), [gate](strict-gate.log).

The integrated activation policy is an explicit product choice, not a change
to benchmark acceptance criteria or proof of improved control. No controller
thresholds were tuned. Hardware energy, application throughput and prediction
accuracy improvements are not established. Earlier native smoke results,
cross-platform differences and development failures remain in the unchanged
[implementation evidence](../cooperative-prediction/README.md).

## Retained failure

The first deployment run failed `upgrade lost prediction setting`: the helper
forwarded a saved coordination mode through sudo as if it were an explicit
installation selection. That incorrectly reapplied the new default during a
plain upgrade. [Failed log](first-attempt-failure.log). Privileged reentry now
receives the actual CLI and revalidates saved configuration and binary
capabilities before stopping the service. The complete deployment suite passes
after this fix. The failed attempt is not counted as a pass.

## Reproduction

```sh
make test
sh tests/test_deploy.sh
make test-integration-abi
python3 tests/check_integration_abi.py --peer ../lertick
# Linux, normal user; only temporary hardware fixtures:
make all test-deployment e2e
# Explicitly authorized disposable systemd VM only:
sudo python3 tests/test_service_systemd.py --allow-systemd-test \
  --special-prefix --daemon build/lerwattd-standalone
sudo python3 tests/test_service_systemd.py --allow-systemd-test \
  --special-prefix --with-lertick --daemon build/lerwattd
```
