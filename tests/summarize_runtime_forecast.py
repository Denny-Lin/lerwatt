#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Audit the six complete runtime cases and apply their frozen accuracy gate."""
import argparse
import json
import math
from pathlib import Path

from verify_runtime_forecast import CASES


def change(old, new):
    if old == 0:
        return 0.0 if new == 0 else None
    return 100 * (new / old - 1)


def summarize(root):
    environment = json.loads((root / "environment.json").read_text())
    if environment.get("smoke_only") is not False or environment["duration_s"] < 1200 or environment["warmup_s"] < 300:
        raise ValueError("runtime confirmation requires full-length non-smoke observations")
    if set(environment["cases"]) != set(CASES) or len(environment["cases"]) != len(CASES):
        raise ValueError("every declared runtime case is required")
    rows = []
    for name in environment["cases"]:
        directory = root / name
        run = json.loads((directory / "run.json").read_text())
        score = json.loads((directory / "scores.json").read_text())
        if not all(run.get(k) is True for k in ("completed", "restored_knobs", "journal_removed")):
            raise ValueError(f"incomplete or unrestored runtime case: {name}")
        if run.get("daemon_exit") != 0 or run.get("simulator_exit") != 0:
            raise ValueError(f"nonzero runtime exit: {name}")
        metrics = score["metrics"]
        if metrics["baseline"]["n"] != metrics["candidate"]["n"] or metrics["baseline"]["n"] <= 0:
            raise ValueError("forecasters must be scored on the same nonempty labels")
        if score["horizon_s"] != 5 or score["warmup_s"] < 300:
            raise ValueError("runtime label horizon or scoring warmup changed")
        row = {"case": name, "samples": metrics["baseline"]["n"], "metrics": metrics,
               "late_labels": score["late_labels"], "temporal_active_points": score["temporal_active_points"],
               "all_knobs_restored": True, "journal_removed": True}
        for kind in ("mae_c", "rmse_c"):
            old, new = metrics["baseline"][kind], metrics["candidate"][kind]
            if not all(math.isfinite(v) and v >= 0 for v in (old, new)):
                raise ValueError("non-finite or negative runtime error")
            row[kind + "_change_percent"] = change(old, new)
        rows.append(row)
    macro = {}
    for kind in ("mae_c", "rmse_c"):
        old = sum(row["metrics"]["baseline"][kind] for row in rows) / len(rows)
        new = sum(row["metrics"]["candidate"][kind] for row in rows) / len(rows)
        macro[kind] = {"baseline": old, "candidate": new, "change_percent": change(old, new)}
    # Only machine-roundoff tolerance, far below telemetry precision; exact
    # mathematical 2%/3% boundaries must not fail after floating division.
    passed = all(v["change_percent"] is not None and v["change_percent"] <= -3 + 1e-9 for v in macro.values()) and all(
        row[kind + "_change_percent"] is not None and row[kind + "_change_percent"] <= 2 + 1e-9
        for row in rows for kind in ("mae_c", "rmse_c"))
    return {"complete_cases": len(rows), "macro": macro, "cases": rows,
            "passed_frozen_accuracy_gate": passed,
            "limits": ["One simulated board through the real daemon/file ABI; no hardware accuracy claim.",
                       "Macro errors weight the six profiles equally; correlated forecasts are not independent trials.",
                       "Three-percent macro improvement and two-percent maximum case regression were declared before collection."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-gate", action="store_true")
    args = parser.parse_args()
    report = summarize(args.input)
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")
    if args.require_gate and not report["passed_frozen_accuracy_gate"]:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
