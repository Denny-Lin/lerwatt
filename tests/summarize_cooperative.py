#!/usr/bin/env python3
"""Paired finite-job audit. Simulated board joules are not hardware energy."""
import argparse
import json
import math
from pathlib import Path

METRICS = ("makespan_s", "simulated_board_j", "job_p99_s", "point_mae_c", "ceiling_moves")


def summarize(rows):
    groups = {}
    for row in rows:
        key = (row["pattern"], row["seed"], row["joint"])
        mode = row["mode"]
        if mode not in ("nominal", "cooperative") or mode in groups.setdefault(key, {}):
            raise ValueError("unknown mode or duplicate paired run")
        if row["jobs_completed"] <= 0 or row["backlog"] != 0:
            raise ValueError("incomplete work cannot pass the finite-job audit")
        if row["offered_work"] <= 0 or not math.isfinite(row["offered_work"]) or row["throttle_steps"] != 0:
            raise ValueError("invalid offered work or simulated kernel throttling")
        if not 0 <= row["peak_c"] <= 84.05:
            raise ValueError("startup-inclusive thermal limit violated")
        for field in (*METRICS, "control_cpu_s", "forecast_points", "correction_queries", "queries"):
            if not isinstance(row[field], (int, float)) or not math.isfinite(row[field]) or row[field] < 0:
                raise ValueError(f"invalid measurement: {field}")
        groups[key][mode] = row
    if not groups:
        raise ValueError("no paired runs")
    pairs = []
    for key, modes in sorted(groups.items()):
        if set(modes) != {"nominal", "cooperative"}:
            raise ValueError("missing matched controller run")
        a, b = modes["nominal"], modes["cooperative"]
        if a["jobs_completed"] != b["jobs_completed"]:
            raise ValueError("different completed work between controllers")
        if abs(a["offered_work"] - b["offered_work"]) > 1e-6:
            raise ValueError("different offered work between controllers")
        delta = {f: b[f] - a[f] for f in METRICS}
        changes = {f: 100 * delta[f] / a[f] if a[f] else None for f in METRICS}
        pairs.append({"pattern": key[0], "seed": key[1], "joint": key[2],
                      "nominal": a, "cooperative": b, "delta": delta, "percent_change": changes,
                      "regressions": [f for f in METRICS if delta[f] > 1e-6]})
    return {"paired_runs": len(pairs), "all_work_completed": True,
            "pairs_with_regressions": sum(bool(p["regressions"]) for p in pairs),
            "corrected_queries": sum(p["cooperative"]["correction_queries"] for p in pairs),
            "pairs": pairs,
            "limits": ["One simulated board model; no hardware energy or Linux superiority claim.",
                       "Finite FIFO work is fully drained; historical served percentages are not used.",
                       "Correction queries are evaluations, not distinct decisions or savings.",
                       "Controller CPU time is simulator-harness measurement, not a real-time guarantee.",
                       "Descriptive paired differences, not statistical significance."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-nonregression", action="store_true")
    args = parser.parse_args()
    report = summarize([json.loads(line) for line in args.input.read_text().splitlines() if line.strip()])
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")
    if args.require_nonregression and report["pairs_with_regressions"]:
        raise SystemExit("strict nonregression gate failed; original measurements remain in the report")


if __name__ == "__main__":
    main()
