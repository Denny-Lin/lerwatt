#!/usr/bin/env python3
"""Deployment regressions through temporary files; never use host CPU controls."""
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


DAEMON = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/lerwattd-standalone").resolve())


def put(root, name, value):
    path = root / name.lstrip("/")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(str(value) + "\n")
    return path


def fixture(root, policies=1, temperature=65000):
    put(root, "sys/devices/system/cpu/online", "0" if policies == 1 else f"0-{policies - 1}")
    for cpu in range(policies):
        base = f"sys/devices/system/cpu/cpufreq/policy{cpu}"
        for name, value in {
            "related_cpus": cpu, "scaling_driver": "intel_pstate",
            "scaling_governor": "powersave", "cpuinfo_min_freq": 400000,
            "cpuinfo_max_freq": 4800000, "scaling_min_freq": 400000,
            "scaling_max_freq": 4800000, "scaling_cur_freq": 2400000,
        }.items():
            put(root, f"{base}/{name}", value)
        put(root, f"sys/devices/system/cpu/cpu{cpu}/topology/physical_package_id", 0)
    put(root, "sys/class/thermal/thermal_zone0/type", "x86_pkg_temp")
    put(root, "sys/class/thermal/thermal_zone0/temp", temperature)
    put(root, "sys/class/thermal/thermal_zone0/trip_point_0_type", "passive")
    put(root, "sys/class/thermal/thermal_zone0/trip_point_0_temp", 85000)
    put(root, "proc/stat", "cpu 1 0 0 20 0 0 0 0\n" + "\n".join(
        f"cpu{i} 1 0 0 20 0 0 0 0" for i in range(policies)))


def invoke(root, *args):
    return subprocess.run([DAEMON, "--sysroot", str(root), "--no-contract", *args],
                          capture_output=True, text=True, timeout=10)


def controls(root):
    values = {}
    for path in root.glob("sys/devices/system/cpu/cpufreq/policy*/scaling_*_freq"):
        if path.name == "scaling_cur_freq":
            continue
        value = path.read_text().strip()
        assert value.isascii() and value.isdecimal() and int(value) > 0
        values[str(path.relative_to(root))] = int(value)
    return values


def raw_controls(root):
    return {name: (root / name).read_bytes() for name in controls(root)}


def wait_for(predicate, message):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (FileNotFoundError, ValueError):
            pass  # A regular-file fixture can be between truncate and write.
        time.sleep(.02)
    raise AssertionError(message)


def test_service_watchdog(work, compiler):
    """Exercise the real control loop against a local notify socket/fixtures."""
    root = work / "service-cpu"
    fixture(root, temperature=84000)
    original = controls(root)
    socket_path = work / "service-notify.sock"
    environment = os.environ.copy()
    environment.pop("WATCHDOG_PID", None)
    environment["NOTIFY_SOCKET"] = str(socket_path)
    environment["WATCHDOG_USEC"] = "400000"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    server.bind(str(socket_path))
    server.settimeout(2)

    def start(label, *extra, env=None):
        errors = open(work / f"service-{label}.err", "w+")
        process = subprocess.Popen(
            [DAEMON, "--sysroot", str(root), "--no-contract", "--watchdog",
             "--duration", "0", "--period-ms", "10", *extra],
            env=env or environment, stdout=subprocess.DEVNULL, stderr=errors)
        return process, errors

    def collect(errors):
        errors.seek(0)
        message = errors.read()
        errors.close()
        return message

    def ready():
        message = server.recv(4096).decode()
        assert "READY=1" in message and "WATCHDOG=1" in message, message
        assert all(value < 4800000 for name, value in controls(root).items()
                   if "scaling_max_freq" in name), "readiness preceded successful actuation"

    def drain():
        server.settimeout(.01)
        try:
            while True:
                server.recv(4096)
        except socket.timeout:
            pass
        server.settimeout(2)

    try:
        # Invalid service configuration must never signal readiness.
        bad_environment = environment.copy()
        bad_environment["WATCHDOG_PID"] = "2147483647"
        result = subprocess.run(
            [DAEMON, "--sysroot", str(root), "--no-contract", "--watchdog"],
            env=bad_environment, capture_output=True, text=True, timeout=5)
        assert result.returncode != 0 and "watchdog environment" in result.stderr
        assert controls(root) == original
        server.settimeout(.05)
        try:
            server.recv(4096)
            raise AssertionError("invalid watchdog environment emitted readiness")
        except socket.timeout:
            pass
        server.settimeout(2)

        process, errors = start("normal")
        try:
            ready()
            heartbeat = server.recv(4096).decode()
            assert "WATCHDOG=1" in heartbeat and "READY=" not in heartbeat
            assert process.poll() is None, "duration 0 unexpectedly expired"
            process.terminate()
            assert process.wait(timeout=5) == 0
            assert controls(root) == original
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            collect(errors)
        drain()

        # No background notifier may keep a frozen control loop alive. Mimic
        # systemd's final SIGKILL, then ExecStopPost's restoration step.
        process, errors = start("frozen")
        try:
            ready()
            process.send_signal(signal.SIGSTOP)
            stopped_pid, status = os.waitpid(process.pid, os.WUNTRACED)
            assert stopped_pid == process.pid and os.WIFSTOPPED(status)
            drain()
            server.settimeout(.5)
            try:
                server.recv(4096)
                raise AssertionError("frozen worker continued watchdog heartbeats")
            except socket.timeout:
                pass
            server.settimeout(2)
            process.kill()
            process.wait(timeout=5)
            result = invoke(root, "--restore")
            assert result.returncode == 0, result.stderr
            assert controls(root) == original
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            collect(errors)
        drain()

        # Newly online CPUs invalidate the discovery snapshot even when the
        # already-controlled CPUs still report valid statistics.
        process, errors = start("hotplug")
        try:
            ready()
            put(root, "sys/devices/system/cpu/online", "0-1")
            assert process.wait(timeout=5) != 0
            assert controls(root) == original
            assert "online CPU topology changed" in collect(errors)
            errors = None
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            if errors:
                collect(errors)
            put(root, "sys/devices/system/cpu/online", "0")
        drain()

        # Make a checkpoint read consume six seconds of both service clocks.
        # This is not suspend: their offset stays unchanged, so only a fresh
        # observation check immediately before actuation can reject the plan.
        private_state = work / "service-private-state"
        private_state.mkdir(mode=0o700)
        delayed_model = private_state / "model.delay"
        delayed_model.write_bytes(bytes(80))
        delayed_model.chmod(0o600)
        delay_source = work / "checkpoint-delay.c"
        delay_library = work / "checkpoint-delay.so"
        delay_source.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int delayed;
ssize_t read(int fd, void *buffer, size_t length)
{
    static ssize_t (*real_read)(int, void *, size_t);
    char descriptor[64], target[4096];
    if (!real_read) real_read = dlsym(RTLD_NEXT, "read");
    ssize_t result = real_read(fd, buffer, length);
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    ssize_t size = readlink(descriptor, target, sizeof(target) - 1);
    if (size > 0) {
        target[size] = 0;
        const char *name = strrchr(target, '/');
        if (result > 0 && name && !strcmp(name, "/model.delay")) delayed = 1;
    }
    return result;
}
int clock_gettime(clockid_t clock, struct timespec *value)
{
    static int (*real_clock)(clockid_t, struct timespec *);
    if (!real_clock) real_clock = dlsym(RTLD_NEXT, "clock_gettime");
    int result = real_clock(clock, value);
    if (!result && delayed && (clock == CLOCK_MONOTONIC || clock == CLOCK_BOOTTIME))
        value->tv_sec += 6;
    return result;
}
''')
        subprocess.run(compiler + ["-shared", "-fPIC", "-std=c11", "-o", str(delay_library),
                                   str(delay_source), "-ldl"], check=True)
        delay_environment = environment.copy()
        delay_environment["LD_PRELOAD"] = str(delay_library)
        process, errors = start("checkpoint-delay", "--model-state", str(delayed_model),
                                env=delay_environment)
        try:
            assert process.wait(timeout=5) != 0
            assert controls(root) == original, "stale observation changed CPU controls"
            assert "CPU observation became stale before actuation" in collect(errors)
            errors = None
            server.settimeout(.05)
            try:
                message = server.recv(4096).decode()
                raise AssertionError(f"stale observation emitted readiness: {message}")
            except socket.timeout:
                pass
            server.settimeout(2)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
                invoke(root, "--restore")
            if errors:
                collect(errors)
        drain()

        # A resume creates a BOOTTIME/MONOTONIC offset. Add two seconds only
        # after the first successful cycle, without actually suspending a host.
        source = work / "resume-fault.c"
        library = work / "resume-fault.so"
        source.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <time.h>
int clock_gettime(clockid_t clock, struct timespec *value)
{
    static int (*real_clock)(clockid_t, struct timespec *);
    static unsigned boots;
    if (!real_clock) real_clock = dlsym(RTLD_NEXT, "clock_gettime");
    int result = real_clock(clock, value);
    if (!result && clock == CLOCK_BOOTTIME && ++boots > 3) value->tv_sec += 2;
    return result;
}
''')
        subprocess.run(compiler + ["-shared", "-fPIC", "-std=c11", "-o", str(library),
                                   str(source), "-ldl"], check=True)
        resume_environment = environment.copy()
        resume_environment["LD_PRELOAD"] = str(library)
        process, errors = start("resume", env=resume_environment)
        try:
            assert process.wait(timeout=5) != 0
            assert controls(root) == original
            assert "service resumed or control observations became stale" in collect(errors)
            errors = None
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            if errors:
                collect(errors)
        drain()

        # Once ready, a broken notify channel must stop control and recover.
        process, errors = start("lost-manager")
        try:
            ready()
            server.close()
            socket_path.unlink()
            assert process.wait(timeout=5) != 0
            assert controls(root) == original
            assert "watchdog notification failed" in collect(errors)
            errors = None
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            if errors:
                collect(errors)
    finally:
        server.close()
        socket_path.unlink(missing_ok=True)


def main():
    for args in [("--duration", "nan"), ("--duration", "-1"),
                 ("--duration", "1s"), ("--period-ms", "100x"),
                 ("--period-ms", "4294967396"), ("--soft-limit", "inf"),
                 ("--soft-limit", "0"), ("--restore", "--dry-run"),
                 ("--describe", "--check"), ("--watchdog", "--check"),
                 ("--watchdog", "--dry-run"), ("--model-state", "relative"),
                 ("--model-state", "/tmp/model", "--describe"), ("unexpected",)]:
        result = subprocess.run([DAEMON, *args], capture_output=True, timeout=5)
        assert result.returncode > 0, args

    with tempfile.TemporaryDirectory(prefix="lerwatt-deployment-") as directory:
        work = Path(directory)
        # A native standalone build must not depend on the modern BPF UAPI.
        # Poison that header to catch accidental includes outside its guard.
        poison = work / "headers/linux"
        poison.mkdir(parents=True)
        (poison / "bpf.h").write_text("#error standalone must not include linux/bpf.h\n")
        source = Path(__file__).resolve().parents[1]
        compiler = shlex.split(os.environ.get("CC", "cc"))
        test_service_watchdog(work, compiler)
        subprocess.run(compiler + ["-std=c11", "-D_GNU_SOURCE", "-DLW_NO_BPF",
                                  "-I", str(poison.parent), "-I", str(source / "include"),
                                  "-I", str(source / "src"), "-c", str(source / "src/contract.c"),
                                  "-o", str(work / "standalone-contract.o")], check=True)
        root = work / "intel"
        fixture(root, policies=16, temperature=84000)
        original = controls(root)
        original_bytes = raw_controls(root)
        result = invoke(root, "--check")
        assert result.returncode == 0, result.stderr
        assert len(original) == 32
        assert raw_controls(root) == original_bytes, "check changed a control file"
        assert not (root / "run").exists(), "check created runtime state"

        result = invoke(root, "--policy", "mpc", "--dry-run", "--duration", ".05")
        assert result.returncode == 0, result.stderr
        assert controls(root) == original
        assert raw_controls(root) == original_bytes, "preview changed a control file"
        assert not (root / "run").exists(), "preview created a journal or lock"

        # A short duration must not leave controls active for a full long
        # period. Give startup/recovery ample slack, while rejecting 10 s.
        started = time.monotonic()
        result = invoke(root, "--duration", ".15", "--period-ms", "10000")
        elapsed = time.monotonic() - started
        assert result.returncode == 0, result.stderr
        assert elapsed >= .1, f"duration .15 ended prematurely: {elapsed:.3f}s"
        assert elapsed < 3, f"duration .15 waited for the 10 s control period: {elapsed:.2f}s"
        assert controls(root) == original
        assert not (root / "run/lerwatt/restore.journal").exists()

        # clock_nanosleep returns an errno value directly. A permanent error
        # must stop and recover, rather than turning control into a busy loop.
        fault_source = work / "sleep-fault.c"
        fault_library = work / "sleep-fault.so"
        fault_source.write_text('''#include <errno.h>
#include <time.h>
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                    struct timespec *remaining)
{
    (void)clock; (void)flags; (void)request; (void)remaining;
    return EINVAL;
}
''')
        subprocess.run(compiler + ["-shared", "-fPIC", "-std=c11", "-D_GNU_SOURCE",
                                  "-o", str(fault_library), str(fault_source)], check=True)
        fault_environment = os.environ.copy()
        fault_environment["LD_PRELOAD"] = str(fault_library) + (
            ":" + fault_environment["LD_PRELOAD"] if fault_environment.get("LD_PRELOAD") else "")
        result = subprocess.run([DAEMON, "--sysroot", str(root), "--no-contract",
                                 "--duration", "1", "--period-ms", "10"],
                                env=fault_environment, capture_output=True, text=True, timeout=5)
        assert result.returncode != 0 and "sleep failed" in result.stderr, result.stderr
        assert controls(root) == original
        assert not (root / "run/lerwatt/restore.journal").exists()

        stderr = open(work / "active.err", "w+")
        # Regular-file fixtures expose a truncate/write gap that real sysfs
        # attribute reads do not have. Keep limits stable during concurrent
        # preview reads after the complete first control pass.
        process = subprocess.Popen([DAEMON, "--sysroot", str(root), "--no-contract",
                                    "--period-ms", "10000", "--duration", "30"], stderr=stderr)
        try:
            cap = root / "sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq"
            wait_for(lambda: all(int(path.read_text()) < 4800000 for path in
                                 root.glob("sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq")),
                     "continuous driver could not lower its frequency ceiling")
            assert process.poll() is None
            for args in [("--duration", ".05"), ("--restore",)]:
                result = invoke(root, *args)
                assert result.returncode != 0 and "lock" in result.stderr.lower(), result.stderr
            result = invoke(root, "--dry-run", "--duration", ".05")
            assert result.returncode == 0, result.stderr
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=5) == 0
            assert controls(root) == original
            assert not (root / "run/lerwatt/restore.journal").exists()
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            stderr.close()

        # Inspection must not recover a leftover journal; active control must
        # refuse to discard an invalid recovery record.
        journal = put(root, "run/lerwatt/restore.journal", "invalid recovery record")
        saved = journal.read_bytes()
        for args in [("--check",), ("--dry-run", "--duration", ".03")]:
            invoke(root, *args)
            assert journal.read_bytes() == saved
            assert controls(root) == original
        result = invoke(root, "--duration", ".03")
        assert result.returncode != 0 and "restor" in result.stderr.lower()
        assert journal.read_bytes() == saved
        assert controls(root) == original
        journal.unlink()

        result = invoke(root, "--duration", ".03", "--telemetry", str(work))
        assert result.returncode != 0 and "telemetry" in result.stderr
        assert controls(root) == original

        # Privileged logs must create new files, never append to a planted
        # symlink, hardlink, FIFO, directory or existing unrelated file.
        protected = work / "unrelated-file"
        protected.write_text("keep this unchanged\n")
        symlink = work / "telemetry-symlink"
        symlink.symlink_to(protected)
        hardlink = work / "telemetry-hardlink"
        os.link(protected, hardlink)
        fifo = work / "telemetry-fifo"
        os.mkfifo(fifo)
        for output in [protected, symlink, hardlink, fifo]:
            result = invoke(root, "--duration", ".03", "--telemetry", str(output))
            assert result.returncode != 0 and "telemetry" in result.stderr
            assert protected.read_text() == "keep this unchanged\n"
            assert controls(root) == original
        output = work / "fresh-telemetry.jsonl"
        result = invoke(root, "--duration", ".03", "--telemetry", str(output))
        assert result.returncode == 0, result.stderr
        assert output.read_text().startswith('{"t":')
        assert output.stat().st_mode & 0o777 == 0o600

        # Maintenance must own the same controller lock until all mutations
        # finish, including when only its supervising daemon is killed.
        entered, release = work / "maintenance-entered", work / "maintenance-release"
        maintenance = work / "maintenance.sh"
        maintenance.write_text('#!/bin/sh\n: > "$1"\nwhile [ ! -e "$2" ]; do sleep .02; done\n')
        maintenance.chmod(0o700)
        process = subprocess.Popen([DAEMON, "--sysroot", str(root), "--no-contract",
                                    "--restore", "--", str(maintenance), str(entered), str(release)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_for(entered.exists, "maintenance command did not start")
            for args in [("--restore",), ("--duration", ".03")]:
                result = invoke(root, *args)
                assert result.returncode != 0 and "lock" in result.stderr.lower()
            process.kill()
            process.wait(timeout=5)
            result = invoke(root, "--restore")
            assert result.returncode != 0 and "lock" in result.stderr.lower()
        finally:
            release.touch()
            if process.poll() is None:
                process.wait(timeout=5)
        wait_for(lambda: invoke(root, "--restore").returncode == 0,
                 "maintenance command did not release its lock")

        # Losing a required temperature must stop active control and restore.
        sensor = root / "sys/class/thermal/thermal_zone0/temp"
        stderr = open(work / "sensor-loss.err", "w+")
        process = subprocess.Popen([DAEMON, "--sysroot", str(root), "--no-contract",
                                    "--period-ms", "10", "--duration", "30"], stderr=stderr)
        try:
            wait_for(lambda: int(cap.read_text()) < 4800000, "second trial did not start")
            sensor.unlink()
            assert process.wait(timeout=5) != 0
            assert controls(root) == original
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            stderr.close()

        # An explicitly selected CPU sensor works even without automatic labels.
        override = put(root, "sys/class/hwmon/hwmon7/temp1_input", 65000)
        shutil.rmtree(root / "sys/class/thermal")
        assert invoke(root, "--check").returncode != 0
        result = invoke(root, "--check", "--temperature-sensor",
                        "/sys/class/hwmon/hwmon7/temp1_input")
        assert result.returncode == 0, result.stderr
        assert override.exists()

        # Reject malformed hardware metadata before any actuation.
        for label, attribute, value in [
            ("cpu-overflow", "related_cpus", "256"),
            ("zero-frequency", "cpuinfo_max_freq", "0"),
            ("invalid-frequency", "cpuinfo_max_freq", "4800000oops"),
        ]:
            bad = work / label
            fixture(bad)
            put(bad, f"sys/devices/system/cpu/cpufreq/policy0/{attribute}", value)
            before = controls(bad)
            result = invoke(bad, "--check")
            assert result.returncode > 0, (label, result.stderr)
            assert controls(bad) == before
            assert not (bad / "run").exists()

    print("daemon deployment: CLI, read-only checks, 16-policy range control, locking, recovery and sensor loss passed")


if __name__ == "__main__":
    main()
