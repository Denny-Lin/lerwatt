#!/usr/bin/env python3
import copy
import unittest

from summarize_cooperative import summarize


class PairedAudit(unittest.TestCase):
    def rows(self):
        base = {"pattern": 0, "seed": 42, "joint": False, "mode": "nominal",
                "jobs_completed": 10, "backlog": 0, "peak_c": 81, "makespan_s": 10,
                "offered_work": 100, "throttle_steps": 0,
                "simulated_board_j": 30, "job_p99_s": 1, "point_mae_c": .5,
                "ceiling_moves": 2, "control_cpu_s": .01, "forecast_points": 50,
                "correction_queries": 0, "queries": 100}
        candidate = dict(base, mode="cooperative", correction_queries=3)
        return [base, candidate]

    def test_detects_latency_regression_despite_lower_energy_and_mae(self):
        rows = self.rows()
        rows[1].update(simulated_board_j=29, point_mae_c=.4, makespan_s=11, job_p99_s=1.2)
        report = summarize(rows)
        self.assertEqual(report["pairs_with_regressions"], 1)
        self.assertEqual(report["pairs"][0]["regressions"], ["makespan_s", "job_p99_s"])
        self.assertAlmostEqual(report["pairs"][0]["percent_change"]["makespan_s"], 10)

    def test_rejects_incomplete_or_mismatched_work(self):
        for change in ({"backlog": 1}, {"jobs_completed": 9}, {"offered_work": 101},
                       {"throttle_steps": 1}, {"peak_c": 84.2}, {"point_mae_c": float("nan")}):
            rows = self.rows(); rows[1].update(change)
            with self.assertRaises(ValueError): summarize(rows)

    def test_requires_unique_complete_pairs(self):
        rows = self.rows()
        with self.assertRaises(ValueError): summarize(rows[:1])
        with self.assertRaises(ValueError): summarize(rows + [copy.deepcopy(rows[1])])
        self.assertEqual(summarize(rows)["pairs_with_regressions"], 0)


if __name__ == "__main__":
    unittest.main()
