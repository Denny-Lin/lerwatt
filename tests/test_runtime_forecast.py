#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent fixtures verify future-label alignment in the runtime scorer."""
import json
from pathlib import Path
import tempfile
import unittest

from verify_runtime_forecast import score, knobs


class RuntimeForecastScoring(unittest.TestCase):
    def test_all_actuator_filenames_are_snapshotted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = ["cpufreq/policy0/scaling_min_freq", "cpufreq/policy0/scaling_max_freq",
                     "cpu0/power/pm_qos_resume_latency_us", "cpu0/cpuidle/state1/disable"]
            for name in files:
                p = root / "sys/devices/system/cpu" / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text("0\n")
            snapshot = knobs(root)
            self.assertEqual(len(snapshot), 4)
            self.assertIn("sys/devices/system/cpu/cpu0/power/pm_qos_resume_latency_us", snapshot)

    def fixture(self):
        rows = []
        for i in range(201):
            hot = i / 10
            correction = 5 if i >= 50 else 0
            rows.append({"t": hot, "t_ns": i * 100000000, "clusters": [{
                "id": 0, "temp_c": hot, "forecast_c": hot + correction,
                "forecast_base_c": hot, "forecast_correction_c": correction,
                "temporal_labels": max(0, i - 49), "forecast_weights": [1, 0, 0],
                "forecast_source": "temporal" if correction else "persistence",
            }]})
        return rows

    def test_restoration_compares_integer_values_without_hiding_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq"
            path.parent.mkdir(parents=True)
            path.write_text("2400000\n")
            original = knobs(root)
            path.write_text("2400000")
            self.assertEqual(knobs(root), original)
            path.write_text("1800000\n")
            self.assertNotEqual(knobs(root), original)
            for invalid in ("", "2400000 garbage", "2400000\n1800000", "nan"):
                path.write_text(invalid)
                with self.assertRaisesRegex(ValueError, "invalid integer"):
                    knobs(root)

    def evaluate(self, rows, warmup=5):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "telemetry.jsonl"
            path.write_text("".join(json.dumps(row) + "\n" for row in rows))
            return score(path, warmup)

    def test_exact_nanosecond_alignment(self):
        result = self.evaluate(self.fixture())
        self.assertEqual(result["metrics"]["baseline"]["n"], 101)
        self.assertEqual(result["metrics"]["baseline"]["mae_c"], 5)
        self.assertAlmostEqual(result["metrics"]["candidate"]["mae_c"], 0)
        self.assertEqual(result["maximum_lateness_s"], 0)

    def test_rejects_unearned_feedback_and_inconsistent_correction(self):
        rows = self.fixture()
        rows[1]["clusters"][0]["temporal_labels"] = 1
        with self.assertRaisesRegex(ValueError, "before its five-second"):
            self.evaluate(rows)
        rows = self.fixture()
        rows[100]["clusters"][0]["forecast_correction_c"] = 0
        with self.assertRaisesRegex(ValueError, "does not reconstruct"):
            self.evaluate(rows)

    def test_does_not_score_late_labels_or_duplicate_clocks(self):
        rows = self.fixture()[::100]
        with self.assertRaisesRegex(ValueError, "no eligible"):
            self.evaluate(rows, 0)
        rows = self.fixture()
        rows[30]["t_ns"] = rows[29]["t_ns"]
        with self.assertRaisesRegex(ValueError, "non-monotonic"):
            self.evaluate(rows)


if __name__ == "__main__":
    unittest.main()
