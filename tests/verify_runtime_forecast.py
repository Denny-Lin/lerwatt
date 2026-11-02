#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run/score real Linux daemon point forecasts through an explicit simulator ABI.

Both forecast columns are issued on the same live trajectory. This measures
the daemon/file-ABI path, not real-board accuracy or a throughput advantage.
All test roots and logs are retained. No existing simulator root is reused.
"""
import argparse
from bisect import bisect_left
from contextlib import ExitStack
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "period142-passive-30C": ("periodic", "passive", 30, 14.2, 11071),
    "sine11-fan-15C": ("sine", "fan", 15, 11, 11177),
    "phase-reset66-passive-30C": ("phase", "passive", 30, 6.6, 11273),
    "random7-bare-20C": ("random", "bare", 20, 7, 11161),
    "steps-bare-15C": ("steps", "bare", 15, 0, 11321),
    "sparse31-fan-35C": ("sparse", "fan", 35, 31, 11131),
}


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def demand(kind, period, seed, t, cpu):
    cluster = 0 if cpu < 4 else 1 if cpu < 6 else 2
    if kind == "periodic":
        return 1 if t % period < period / 2 else .05
    if kind == "sine":
        return .5 + .45 * math.sin(t * (2 * math.pi / period) + cpu / 4)
    if kind == "sparse":
        return 1 if (t + cluster * .4) % period < 1.2 else .02
    if kind == "random":
        value = ((int(t / period) + seed + cluster * 17) * 6364136223846793005 + 1) & ((1 << 64) - 1)
        return .1 + .9 * ((value >> 32) % 1000) / 999
    if kind == "phase":
        value = ((int(t / 137) + seed) * 6364136223846793005 + 1) & ((1 << 64) - 1)
        phase = ((value >> 32) % 1000) * period / 1000
        return 1 if (t + phase) % period < period / 2 else .05
    return (int(t / 120) % 4 + 1) * .25


def score(path, warmup=300):
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    times = [row.get("t_ns", round(row["t"] * 1e9)) for row in rows]
    if not rows or any(times[i] <= times[i - 1] for i in range(1, len(rows))):
        raise ValueError("missing or non-monotonic daemon observations")
    errors = {"baseline": [], "candidate": []}
    active, late, eligible = 0, 0, 0
    by_cluster = {}
    first_feedback = None
    max_lateness = 0
    for i, row in enumerate(rows):
        for c in row["clusters"]:
            if c["temporal_labels"]:
                first_feedback = row["t"] if first_feedback is None else min(first_feedback, row["t"])
            if first_feedback is not None and first_feedback < 4.99:
                raise ValueError("temporal feedback appeared before its five-second horizon")
            weights = c["forecast_weights"]
            if any(not math.isfinite(w) or w < -1e-5 for w in weights) or abs(sum(weights) - 1) > 3e-5:
                raise ValueError("base mixture weights are not normalized")
            if abs(c["forecast_c"] - c["forecast_base_c"] - c["forecast_correction_c"]) > .0021:
                raise ValueError("published correction does not reconstruct the point forecast")
        if times[i] < warmup * 1e9:
            continue
        j = bisect_left(times, times[i] + 5000000000, lo=i + 1)
        if j == len(rows):
            continue  # No future label at the end of a finite run.
        eligible += len(row["clusters"])
        lateness = (times[j] - times[i] - 5000000000) / 1e9
        if lateness > .25:
            late += len(row["clusters"])
            continue
        max_lateness = max(max_lateness, lateness)
        labels = {c["id"]: c for c in rows[j]["clusters"]}
        for c in row["clusters"]:
            y = labels[c["id"]]["temp_c"]
            if not all(math.isfinite(v) for v in (y, c["forecast_base_c"], c["forecast_c"])):
                raise ValueError("non-finite forecast/label")
            group = by_cluster.setdefault(c["id"], {"baseline": [], "candidate": []})
            for key, field in (("baseline", "forecast_base_c"), ("candidate", "forecast_c")):
                error = c[field] - y
                errors[key].append(error)
                group[key].append(error)
            active += c["forecast_source"] == "temporal"
    if not errors["baseline"]:
        raise ValueError("no eligible runtime forecast labels")
    if first_feedback is None:
        raise ValueError("no temporal feedback was observed")

    def metric(values):
        return {"n": len(values), "mae_c": sum(abs(x) for x in values) / len(values),
                "rmse_c": math.sqrt(sum(x * x for x in values) / len(values))}

    return {"warmup_s": warmup, "horizon_s": 5, "eligible": eligible, "late_labels": late,
            "maximum_lateness_s": max_lateness, "first_feedback_s": first_feedback,
            "temporal_active_points": active, "metrics": {k: metric(v) for k, v in errors.items()},
            "clusters": {k: {name: metric(v) for name, v in group.items()} for k, group in by_cluster.items()},
            "limits": ["One simulated board through the real daemon/file ABI; no hardware accuracy claim.",
                       "First later sensor label within 250ms; no interpolation or future data at issue time.",
                       "Overlapping five-second forecasts are correlated; no per-point confidence interval."]}


def knobs(root):
    names = {"scaling_min_freq", "scaling_max_freq", "disable", "pm_qos_resume_latency_us"}
    snapshot = {}
    for p in (root / "sys/devices/system/cpu").rglob("*"):
        if not p.is_file() or p.name not in names:
            continue
        value = p.read_text().strip()
        if not re.fullmatch(r"-?[0-9]+", value):
            raise ValueError(f"invalid integer actuator value: {p}")
        # Real sysfs reports a trailing newline regardless of the write.
        # The simulator's regular files preserve the writer's formatting.
        snapshot[str(p.relative_to(root))] = str(int(value))
    return snapshot


def stop(process):
    if process is not None and process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def run_case(output, name, duration, warmup):
    directory = output / name
    directory.mkdir()
    root = directory / "root"
    kind, cooling, ambient, period, seed = CASES[name]
    trace = directory / "load.txt"
    with trace.open("w") as f:
        for tick in range(int((duration + 20) * 10) + 1):
            t = tick / 10
            f.write(f"{t:.1f} " + " ".join(f"{demand(kind, period, seed, t, cpu):.8f}" for cpu in range(8)) + "\n")
    sim_command = [str(ROOT / "build/lerwatt-sim"), "--root", str(root), "--load", f"trace:{trace}",
                   "--cooling", cooling, "--ambient", str(ambient), "--seed", str(seed), "--no-cpuperf",
                   "--duration", str(duration + 20), "--log", str(directory / "truth.jsonl")]
    daemon_command = [str(ROOT / "build/lerwattd"), "--sysroot", str(root), "--no-contract", "--policy", "mpc",
                      "--forecast", "temporal", "--duration", str(duration), "--telemetry", str(directory / "telemetry.jsonl")]
    evidence = {"case": name, "simulation": True, "simulator_command": sim_command,
                "daemon_command": daemon_command, "started_unix": time.time(), "completed": False}
    save(directory / "run.json", evidence)
    sim, daemon = None, None
    originals = None
    try:
        with ExitStack() as stack:
            sim = subprocess.Popen(sim_command, stdout=stack.enter_context((directory / "sim.out").open("w")),
                                   stderr=stack.enter_context((directory / "sim.err").open("w")))
            deadline = time.monotonic() + 15
            while not (root / ".lerwatt-sim").exists():
                if sim.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("simulator did not start; see sim.err")
                time.sleep(.05)
            originals = knobs(root)
            if not originals:
                raise RuntimeError("no simulated actuator knobs discovered")
            save(directory / "original-knobs.json", originals)
            usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
            daemon = subprocess.Popen(daemon_command, stdout=stack.enter_context((directory / "daemon.out").open("w")),
                                      stderr=stack.enter_context((directory / "daemon.err").open("w")))
            code = daemon.wait(timeout=duration + 30)
            usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
            evidence["daemon_user_cpu_s"] = usage_after.ru_utime - usage_before.ru_utime
            evidence["daemon_system_cpu_s"] = usage_after.ru_stime - usage_before.ru_stime
            if code:
                raise RuntimeError(f"daemon exited {code}; see daemon.err")
            if knobs(root) != originals or (root / "run/lerwatt/restore.journal").exists():
                raise RuntimeError("daemon did not restore every original actuator knob")
            if sim.poll() is not None:
                raise RuntimeError("simulator ended before daemon; see sim.err")
            evidence["restored_knobs"] = True
            # Stop the load as soon as the controller has restored its knobs.
            # Offline JSON scoring must not extend the uncontrolled hot tail.
            stop(sim)
            result = score(directory / "telemetry.jsonl", warmup)
            if kind in ("periodic", "sine", "phase") and not result["temporal_active_points"]:
                raise RuntimeError("temporal correction never activated on a periodic confirmation case")
            save(directory / "scores.json", result)
            evidence["completed"] = True
    finally:
        stop(daemon)
        stop(sim)
        if originals is not None:
            evidence["restored_knobs"] = knobs(root) == originals
            evidence["journal_removed"] = not (root / "run/lerwatt/restore.journal").exists()
            if not evidence["restored_knobs"] or not evidence["journal_removed"]:
                evidence["completed"] = False
        if evidence["completed"]:
            summary = (directory / "sim.err").read_text()
            bad = re.search(r"(\d+) bad writes", summary)
            throttle = re.search(r"(\d+) throttle steps", summary)
            evidence["simulator_summary"] = summary
            evidence["completed"] = bool(sim.returncode == 0 and bad and throttle and
                                          int(bad[1]) == 0 and int(throttle[1]) == 0)
        evidence.update(finished_unix=time.time(), daemon_exit=daemon.returncode if daemon else None,
                        simulator_exit=sim.returncode if sim else None)
        save(directory / "run.json", evidence)
    if not evidence["completed"]:
        raise RuntimeError("runtime confirmation did not complete safely; see run.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--score", type=Path, help="score one retained telemetry JSONL without running processes")
    parser.add_argument("--duration", type=float, default=1200)
    parser.add_argument("--warmup", type=float, default=300)
    parser.add_argument("--cases", default=",".join(CASES))
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    if args.score:
        print(json.dumps(score(args.score, args.warmup), indent=2))
        return
    if not args.output or os.uname().sysname != "Linux" or os.geteuid() != 0:
        parser.error("running needs Linux root and a fresh --output directory")
    if not all(math.isfinite(v) and v >= 0 for v in (args.duration, args.warmup)) or args.duration <= args.warmup + 5:
        parser.error("finite duration must exceed warmup plus the five-second horizon")
    if not args.smoke and (args.duration < 1200 or args.warmup < 300):
        parser.error("confirmation requires >=1200s duration and >=300s warmup; shorter runs need --smoke")
    names = args.cases.split(",")
    if len(set(names)) != len(names) or set(names) - set(CASES):
        parser.error("unknown or duplicate cases")
    scx_state = Path("/sys/kernel/sched_ext/state")
    if scx_state.exists() and scx_state.read_text().strip() != "disabled":
        parser.error("refusing an active sched_ext scheduler")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    files = [ROOT / "build/lerwattd", ROOT / "build/lerwatt-sim", Path(__file__).resolve()]
    save(output / "environment.json", {"kernel": os.uname().release, "architecture": os.uname().machine,
         "duration_s": args.duration, "warmup_s": args.warmup, "cases": names, "smoke_only": args.smoke,
         "sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}})
    for name in names:
        print(f"START runtime {name}", flush=True)
        run_case(output, name, args.duration, args.warmup)
        print(f"PASS runtime {name}", flush=True)


if __name__ == "__main__":
    main()
