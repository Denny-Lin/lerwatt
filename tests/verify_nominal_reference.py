#!/usr/bin/env python3
"""Compile frozen original MPC and current nominal against the same FIFO plant.

Requires the baseline Git object locally. Only state-field paths and public
symbol names are adapted; the original control and learning expressions stay
unchanged. Builds/outputs belong to this explicitly requested audit directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASELINE = "953a11ee88f53ff9cc5593550f528303047c797e"
EXPECTED = {
    "controller.c": "1bddc374b4e380451203f3d48e2204e481e8586c71b56067e6cbd6b9107a7e9a",
    "learn.c": "8a5fbfc6c93f48f3e40a7abf1b2259c3b1070cdf58bc3a14079fce6402162d64",
    "forecast.c": "21265a764056780555e32f03de4f4d2db0a82ed16557ec5e855b1ef7405c815e",
    "temporal.c": "5a73c46bb8c16857ecaaa1c8725ed54bd3a2033f86ff5bb3d1982e77632d3f64",
}


def run(args, **kwargs):
    return subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline-directory", type=Path,
                        help="optional offline directory with the four SHA-256-pinned original C files")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"baseline": BASELINE, "original_sources": {}, "differences": []}
    with tempfile.TemporaryDirectory(prefix="lerwatt-reference-") as temporary:
        work = Path(temporary)
        sources = []
        for name in ("controller.c", "learn.c", "forecast.c", "temporal.c"):
            original = (args.baseline_directory / name).read_bytes() if args.baseline_directory else \
                subprocess.check_output(["git", "show", BASELINE + ":src/" + name], cwd=ROOT)
            report["original_sources"][name] = hashlib.sha256(original).hexdigest()
            assert report["original_sources"][name] == EXPECTED[name], "frozen reference source mismatch"
            text = original.decode()
            if name == "controller.c":
                for field, replacement in (("tm", "predictor.thermal"),
                                           ("forecaster", "predictor.forecaster"),
                                           ("temporal", "predictor.temporal")):
                    text = re.sub(r"(\.|->)" + field + r"\b", lambda m: m[1] + replacement, text)
            source = work / name
            source.write_text(text)
            sources.append(str(source))
        common = shlex.split(os.environ.get("CC", "cc")) + [
            "-O2", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
            "-Iinclude", "-Isrc", "-Isim", "tests/test_cooperative_loop.c",
            "sim/platform_rk3588.c", "sim/model.c"]
        builds = {
            "original": common + ["-Dlw_ctl_default_params=reference_defaults",
                "-Dlw_ctl_step_group=reference_group", *sources],
            "current": common + ["src/controller.c", "src/learn.c", "src/forecast.c",
                "src/temporal.c", "src/predict.c"],
        }
        for name, command in builds.items():
            binary = work / name
            with (args.output / (name + "-build.log")).open("w") as log:
                run(command + ["-lm", "-o", str(binary)], stdout=log, stderr=subprocess.STDOUT)
            with (args.output / (name + ".jsonl")).open("w") as log:
                run([str(binary), "--full"], stdout=log)
        rows = {}
        for name in builds:
            data = [json.loads(x) for x in (args.output / (name + ".jsonl")).read_text().splitlines()]
            rows[name] = {(x["pattern"], x["seed"], x["joint"]): x for x in data if x["mode"] == "nominal"}
        assert rows["original"].keys() == rows["current"].keys() and len(rows["current"]) == 30
        for key, a in sorted(rows["original"].items()):
            b = rows["current"][key]
            diff = {k: [a[k], b[k]] for k in a if k not in
                    ("queries", "correction_queries", "control_cpu_s") and a[k] != b[k]}
            if diff:
                report["differences"].append({"pattern": key[0], "seed": key[1], "joint": key[2], "fields": diff})
        report["matched_nominal_cases"] = len(rows["current"])
        report["passed"] = not report["differences"]
        report["scope"] = "Same-platform original vs nominal; not cross-platform bitwise reproducibility or performance gain."
    (args.output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["passed"]:
        raise SystemExit("nominal differs from frozen original; see comparison.json")
    print("PASS 30 nominal cases match the frozen original controller and learners")


if __name__ == "__main__":
    main()
