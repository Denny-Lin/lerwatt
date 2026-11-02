#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import tempfile
import unittest

from summarize_runtime_forecast import summarize
from verify_runtime_forecast import CASES


class RuntimeSummary(unittest.TestCase):
    def fixture(self, root):
        (root / "environment.json").write_text(json.dumps({"cases": list(CASES), "smoke_only": False,
                                                          "duration_s": 1200, "warmup_s": 300}))
        for name in CASES:
            p = root / name
            p.mkdir()
            (p / "run.json").write_text(json.dumps({"completed": True, "restored_knobs": True,
                "journal_removed": True, "daemon_exit": 0, "simulator_exit": 0}))
            (p / "scores.json").write_text(json.dumps({"horizon_s": 5, "warmup_s": 300,
                "late_labels": 0, "temporal_active_points": 100,
                "metrics": {"baseline": {"n": 100, "mae_c": 1, "rmse_c": 2},
                            "candidate": {"n": 100, "mae_c": .5, "rmse_c": 1}}}))

    def test_equal_profile_macro_and_individual_regression_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            report = summarize(root)
            self.assertTrue(report["passed_frozen_accuracy_gate"])
            self.assertEqual(report["macro"]["mae_c"]["change_percent"], -50)
            p = root / next(iter(CASES)) / "scores.json"
            score = json.loads(p.read_text())
            score["metrics"]["candidate"]["mae_c"] = 1.03
            p.write_text(json.dumps(score))
            self.assertFalse(summarize(root)["passed_frozen_accuracy_gate"])

    def test_incomplete_or_unmatched_case_is_not_omitted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            p = root / next(iter(CASES)) / "run.json"
            record = json.loads(p.read_text())
            record["journal_removed"] = False
            p.write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, "unrestored"):
                summarize(root)


if __name__ == "__main__":
    unittest.main()
