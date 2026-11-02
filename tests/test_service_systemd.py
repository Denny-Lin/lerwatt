#!/usr/bin/env python3
"""Explicit VM-only PID1 integration; all CPU controls are temporary fixtures.

Creates one uniquely named validation unit. Never starts the production unit,
changes a real CPUFreq setting, reboots, or stops another power manager.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import uuid


def call(*args, check=True, env=None):
    result = subprocess.run(args, text=True, capture_output=True, timeout=50, env=env)
    if check and result.returncode:
        raise RuntimeError(f"command failed: {args!r}\n{result.stdout}\n{result.stderr}")
    return result


def wait_for(predicate, message, seconds=35):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, ValueError, subprocess.CalledProcessError):
            pass
        time.sleep(.05)
    raise AssertionError(message)


def virtual_machine():
    for name in ("/sys/class/dmi/id/product_name", "/sys/class/dmi/id/sys_vendor",
                 "/sys/hypervisor/type", "/proc/device-tree/model"):
        try:
            text = Path(name).read_text().lower()
            if any(word in text for word in ("qemu", "kvm", "linux,dummy-virt")):
                return True
        except OSError:
            pass
    return False


def put(root, name, value):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(str(value) + "\n")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-systemd-test", action="store_true")
    parser.add_argument("--special-prefix", action="store_true",
                        help="also validate executable paths with spaces, percent and dollar")
    parser.add_argument("--daemon", default="build/lerwattd-standalone")
    parser.add_argument("--with-lertick", action="store_true",
                        help="install BPF-capable optional coordination; absent peer must remain independent")
    parser.add_argument("--prediction", choices=("nominal", "cooperative"),
                        help="Override installation default (standalone nominal; --with-lertick cooperative)")
    options = parser.parse_args()
    if not options.allow_systemd_test or sys.platform != "linux" or os.geteuid() != 0 or not virtual_machine():
        parser.error("requires explicit --allow-systemd-test as root in a disposable QEMU/KVM VM")
    if not Path("/run/systemd/system").is_dir():
        parser.error("a running systemd manager is required")
    source = Path(__file__).resolve().parents[1]
    daemon = Path(options.daemon).resolve()
    assert daemon.is_file() and os.access(daemon, os.X_OK), daemon
    token = uuid.uuid4().hex[:12]
    label = "lerwatt-validation-" + token
    unit = label + ".service"
    # /run is commonly mounted noexec. Keep executable fixture wrappers on
    # /var/lib, which is also visible through the unit's PrivateTmp mount.
    work = Path("/var/lib") / ("lw-service-fixture-" + token)
    prefix = Path("/opt") / (label + (' space%$' if options.special_prefix else ''))
    state = Path("/var/lib") / label
    unit_file = Path("/etc/systemd/system") / unit
    work.mkdir(mode=0o700)
    checkout = work / "checkout"
    checkout.mkdir()
    fixture = work / "hardware"
    base = "sys/devices/system/cpu/cpufreq/policy0/"
    for name, value in {
        "related_cpus": "0", "scaling_driver": "intel_pstate",
        "scaling_governor": "powersave", "cpuinfo_min_freq": "400000",
        "cpuinfo_max_freq": "4800000", "scaling_min_freq": "400000",
        "scaling_max_freq": "4800000", "scaling_cur_freq": "2400000",
    }.items():
        put(fixture, base + name, value)
    put(fixture, "sys/devices/system/cpu/cpu0/topology/physical_package_id", "0")
    put(fixture, "sys/devices/system/cpu/online", "0")
    put(fixture, "sys/class/thermal/thermal_zone0/type", "x86_pkg_temp")
    sensor = put(fixture, "sys/class/thermal/thermal_zone0/temp", "84000")
    put(fixture, "sys/class/thermal/thermal_zone0/trip_point_0_type", "passive")
    put(fixture, "sys/class/thermal/thermal_zone0/trip_point_0_temp", "85000")
    put(fixture, "proc/stat", "cpu 1 0 0 20 0 0 0 0\ncpu0 1 0 0 20 0 0 0 0")
    maximum = fixture / (base + "scaling_max_freq")
    minimum = fixture / (base + "scaling_min_freq")
    journal = fixture / "run/lerwatt/restore.journal"
    build = checkout / "build"
    build.mkdir()
    actual = build / "lerwattd-real"
    shutil.copy2(daemon, actual)
    # Test-only wrapper changes only the selected sysroot. Insert before '--'
    # so the lock-held maintenance command and its argv remain intact.
    wrapper = build / ("lerwattd" if options.with_lertick else "lerwattd-standalone")
    wrapper.write_text("#!/usr/bin/python3\nimport os,sys\na=sys.argv[1:]\n"
                       "i=a.index('--') if '--' in a else len(a)\n"
                       f"a[i:i]=['--sysroot',{str(fixture)!r}]\n"
                       f"os.execv({str(actual)!r},[{str(actual)!r},*a])\n")
    wrapper.chmod(0o755)
    put(checkout, "Makefile", "# test-only already-built source tree")
    put(checkout, "src/lerwattd.c", "/* test-only source sentinel */")
    helper = checkout / "deploy.sh"
    text = (source / "deploy.sh").read_text()
    for old, new in (("lerwatt.service", unit), ("/etc/lerwatt", str(work / "config")),
                     ("/var/lib/lerwatt", str(state)), ("StateDirectory=lerwatt\n", f"StateDirectory={label}\n"),
                     ("WatchdogSec=30s", "WatchdogSec=4s"), ("TimeoutStopSec=15s", "TimeoutStopSec=2s"),
                     ("TimeoutAbortSec=15s", "TimeoutAbortSec=2s"),
                     ("RestartSec=10s", "RestartSec=1s")):
        assert old in text, old
        text = text.replace(old, new)
    helper.write_text(text)
    helper.chmod(0o755)
    environment = dict(os.environ, PREFIX=str(prefix), DESTDIR="")

    def property_value(name):
        return call("systemctl", "show", unit, "--property", name, "--value").stdout.strip()

    def current_pid():
        return int(property_value("MainPID") or "0")

    def healthy(previous=0):
        pid = current_pid()
        return pid if pid and pid != previous and property_value("ActiveState") == "active" else 0

    def controls_restored():
        return int(maximum.read_text()) == 4800000 and int(minimum.read_text()) == 400000 and not journal.exists()

    def restored_during_restart(previous):
        return current_pid() != previous and controls_restored()

    result = {}
    try:
        install_arguments = ["--lertick"] if options.with_lertick else []
        if options.prediction:
            install_arguments += ["--prediction", options.prediction]
        expected_prediction = options.prediction or ("cooperative" if options.with_lertick else "nominal")
        installed = call(str(helper), "install", *install_arguments, env=environment)
        assert "running continuously" in installed.stdout, installed.stdout + installed.stderr
        pid = wait_for(healthy, "service never became ready")
        assert call("systemctl", "is-enabled", unit).stdout.strip() == "enabled"
        wait_for(lambda: int(maximum.read_text()) < 4800000, "fixture controls were not applied")
        command = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        duration = command.index(b"--duration")
        assert command[duration + 1] == b"0" and b"--watchdog" in command
        prediction = command.index(b"--prediction")
        assert command[prediction + 1].decode() == expected_prediction
        assert f"prediction={expected_prediction}" in (work / "config/lerwatt.conf").read_text()
        result["installation_prediction_mode"] = expected_prediction
        if options.with_lertick:
            assert b"--no-contract" not in command
            assert "contract=auto" in (work / "config/lerwatt.conf").read_text()
            wait_for(lambda: "Lertick: waiting" in property_value("StatusText"),
                     "absent peer did not report independent waiting state")
            result["optional_coordination_waits_independently"] = True
        time.sleep(2)
        assert current_pid() == pid and healthy()
        result["enabled_unlimited_ready"] = True
        print(f"systemd: install enabled an unlimited ready worker ({expected_prediction})", flush=True)

        # A stopped loop cannot fake progress; real PID1 must expire it,
        # restore controls through ExecStopPost, and start a fresh worker.
        before = int(property_value("NRestarts"))
        os.kill(pid, signal.SIGSTOP)
        wait_for(lambda: restored_during_restart(pid), "watchdog did not restore before restart")
        new_pid = wait_for(lambda: healthy(pid), "watchdog did not restart the stopped worker")
        assert int(property_value("NRestarts")) > before
        wait_for(lambda: int(maximum.read_text()) < 4800000, "watchdog restart did not resume control")
        result["watchdog_restores_and_restarts"] = True
        print("systemd: stopped worker was recovered and restarted", flush=True)

        before = int(property_value("NRestarts"))
        sensor.unlink()
        wait_for(controls_restored, "sensor failure did not restore controls")
        wait_for(lambda: int(property_value("NRestarts")) > before, "sensor failure did not retry")
        sensor.write_text("84000\n")
        pid = wait_for(lambda: healthy(new_pid), "service did not recover when sensor returned")
        wait_for(lambda: int(maximum.read_text()) < 4800000, "recovered sensor did not resume control")
        result["sensor_failure_recovers"] = True
        print("systemd: missing sensor restored controls; returning sensor recovered service", flush=True)

        removed = call(str(prefix / "bin/lerwatt"), "uninstall", env=environment)
        assert "Removed managed tools/unit" in removed.stdout, removed.stdout + removed.stderr
        assert not unit_file.exists() and not (prefix / "bin/lerwatt").exists()
        assert not (prefix / "sbin/lerwattd").exists() and controls_restored()
        time.sleep(2)
        assert property_value("ActiveState") == "inactive"
        result["uninstall_stops_disables_restores"] = True
        print("systemd: uninstall stopped, disabled and restored the validation service", flush=True)
        print(json.dumps({"result": "passed", "checks": result, "hardware": "temporary file fixture"}), flush=True)
    finally:
        call("systemctl", "stop", unit, check=False)
        call("systemctl", "disable", unit, check=False)
        if unit_file.exists():
            unit_file.unlink()
        call("systemctl", "daemon-reload", check=False)
        call("systemctl", "reset-failed", unit, check=False)
        shutil.rmtree(prefix, ignore_errors=True)
        shutil.rmtree(state, ignore_errors=True)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
