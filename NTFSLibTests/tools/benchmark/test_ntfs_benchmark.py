#!/usr/bin/env python3
"""Tests of ntfs_benchmark.py. Standard library only, Python 3.12 or later.

The pure functions of the script are tested with fabricated inputs: no disk, no
process. The self-test of the script and the check of the workflow need more:
the self-test runs against a built NtfsBench (--bench-exe, or the environment
variable NTFS_BENCH_EXE; without either the test is skipped), and the workflow
check reads files of the repository.

Run: python -I test_ntfs_benchmark.py [--bench-exe path/to/NtfsBench]
"""

from __future__ import annotations

import contextlib
import csv
import importlib.util
import io
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "ntfs_benchmark.py"
# Where the executable under test is, when the command line gives it.
BENCH_EXE: str | None = os.environ.get("NTFS_BENCH_EXE") or None


def load_script():
    """Imports the script by path: -I keeps its folder out of sys.path."""
    spec = importlib.util.spec_from_file_location("ntfs_benchmark", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules["ntfs_benchmark"] = module
    spec.loader.exec_module(module)
    return module


nb = load_script()

# One millisecond, in the nanoseconds the script times in.
MS = 1_000_000


class FunctionEvaluator(nb.Evaluator):
    """Times a configuration with a function, and notes every call."""

    def __init__(self, function):
        self.function = function
        self.calls: list[tuple[dict[str, int], int]] = []

    def measure(self, config, run_index):
        self.calls.append((dict(config), run_index))
        return float(self.function(config, run_index))


def searcher_for(function, defaults, limits=None, results=None, memory_cost=None):
    evaluator = FunctionEvaluator(function)
    limits = limits or {name: (1, 1 << 20) for name in defaults}
    kwargs = {}
    if memory_cost is not None:
        kwargs["memory_cost"] = memory_cost
    searcher = nb.Searcher(
        evaluator, results or nb.ResultsFile(None), "M1", limits, defaults, **kwargs
    )
    return searcher, evaluator


class StatsTests(unittest.TestCase):
    def test_median_low_high(self):
        stats = nb.Stats([3.0, 1.0, 2.0])
        self.assertEqual(stats.median, 2.0)
        self.assertEqual(stats.low, 1.0)
        self.assertEqual(stats.high, 3.0)

    def test_median_of_an_even_count_is_the_mean_of_the_middle_two(self):
        self.assertEqual(nb.Stats([1.0, 2.0, 3.0, 10.0]).median, 2.5)

    def test_empty_stats_are_nan_and_print_as_not_available(self):
        stats = nb.Stats([])
        self.assertTrue(math.isnan(stats.median))
        self.assertTrue(math.isnan(stats.low))
        self.assertTrue(math.isnan(stats.high))
        self.assertEqual(stats.text(), "n/a")

    def test_text_gives_the_median_and_the_range(self):
        self.assertEqual(
            nb.Stats([1e9, 2e9, 4e9]).text(), "2.00 s [1.00-4.00]"
        )


class SplitProfileTests(unittest.TestCase):
    def test_a_value_every_pattern_holds_goes_in_common(self):
        section = nb.split_profile(
            {
                "M1": {"reader.max_reads": 16, "mft.window_bytes": 1 << 20},
                "M2": {"reader.max_reads": 16},
            }
        )
        self.assertEqual(
            section,
            {"common": {"reader.max_reads": 16}, "M1": {"mft.window_bytes": 1 << 20}},
        )

    def test_a_value_that_differs_gets_a_section_for_each_pattern(self):
        section = nb.split_profile(
            {"M1": {"reader.max_reads": 16}, "M2": {"reader.max_reads": 8}}
        )
        self.assertEqual(
            section, {"M1": {"reader.max_reads": 16}, "M2": {"reader.max_reads": 8}}
        )

    def test_a_pattern_that_left_the_knob_at_its_default_blocks_common(self):
        # M2 was searched and kept the default: it has no value, so M1's value
        # is not the value of every pattern.
        section = nb.split_profile(
            {"M1": {"reader.max_reads": 16}, "M2": {"mft.max_run_slots": 8}}
        )
        self.assertNotIn("common", section)
        self.assertEqual(section["M1"], {"reader.max_reads": 16})
        self.assertEqual(section["M2"], {"mft.max_run_slots": 8})

    def test_one_pattern_alone_keeps_its_own_section(self):
        section = nb.split_profile({"M1": {"reader.max_reads": 16}})
        self.assertEqual(section, {"M1": {"reader.max_reads": 16}})

    def test_four_patterns_that_agree_go_in_common_once(self):
        found = {pattern: {"reader.max_bytes": 1 << 24} for pattern in ("M1", "M2", "M3", "M4")}
        self.assertEqual(
            nb.split_profile(found), {"common": {"reader.max_bytes": 1 << 24}}
        )

    def test_nothing_found_gives_an_empty_section(self):
        self.assertEqual(nb.split_profile({"M1": {}, "M2": {}}), {})
        self.assertEqual(nb.split_profile({}), {})

    def test_the_names_come_out_in_a_fixed_order(self):
        found = {"M1": {"b": 1, "a": 2}, "M2": {"a": 2, "b": 3}}
        section = nb.split_profile(found)
        self.assertEqual(section["common"], {"a": 2})
        self.assertEqual(section["M1"], {"b": 1})
        self.assertEqual(section["M2"], {"b": 3})


class VerdictTests(unittest.TestCase):
    def test_a_run_as_long_as_its_io_is_io_bound_strongly(self):
        judged = nb.verdict(10.0, 9.8, 4.0)
        self.assertEqual(judged["critical_path"], "I/O (strong)")

    def test_strong_stops_at_one_point_zero_five_times_the_io(self):
        self.assertEqual(nb.verdict(10.5, 10.0, 4.0)["critical_path"], "I/O (strong)")
        self.assertEqual(nb.verdict(10.6, 10.0, 4.0)["critical_path"], "I/O")

    def test_io_stops_at_one_point_one_times_the_io(self):
        self.assertEqual(nb.verdict(11.0, 10.0, 4.0)["critical_path"], "I/O")
        self.assertTrue(
            nb.verdict(11.01, 10.0, 4.0)["critical_path"].startswith("mixed")
        )

    def test_the_thresholds_are_the_documented_ones(self):
        self.assertEqual(nb.CRITICAL_STRONG, 1.05)
        self.assertEqual(nb.CRITICAL_PLAIN, 1.10)

    def test_a_run_as_long_as_its_cpu_is_cpu_bound(self):
        self.assertEqual(nb.verdict(9.5, 5.0, 9.0)["critical_path"], "CPU")
        self.assertEqual(nb.verdict(9.9, 5.0, 9.0)["critical_path"], "CPU")

    def test_a_run_longer_than_both_floors_is_mixed(self):
        self.assertTrue(nb.verdict(14.0, 5.0, 9.0)["critical_path"].startswith("mixed"))
        self.assertTrue(nb.verdict(14.0, 9.0, 5.0)["critical_path"].startswith("mixed"))

    def test_equal_floors_are_not_io_bound(self):
        # The CPU is not below the I/O: the run is called CPU, not I/O.
        self.assertEqual(nb.verdict(10.0, 10.0, 10.0)["critical_path"], "CPU")

    def test_r_is_zero_when_the_phases_run_in_sequence(self):
        self.assertAlmostEqual(nb.verdict(14.0, 5.0, 9.0)["R"], 0.0)

    def test_r_is_one_when_the_cpu_hides_under_the_io(self):
        self.assertAlmostEqual(nb.verdict(9.0, 9.0, 4.0)["R"], 1.0)
        self.assertAlmostEqual(nb.verdict(9.0, 4.0, 9.0)["R"], 1.0)

    def test_r_is_between_zero_and_one_for_a_partial_overlap(self):
        # 3 s of the 4 s of the smaller floor overlap.
        self.assertAlmostEqual(nb.verdict(10.0, 9.0, 4.0)["R"], 0.75)

    def test_r_goes_past_one_when_the_run_beats_the_larger_floor(self):
        self.assertGreater(nb.verdict(8.0, 9.0, 4.0)["R"], 1.0)

    def test_r_is_nan_without_a_floor(self):
        self.assertTrue(math.isnan(nb.verdict(5.0, 0.0, 4.0)["R"]))
        self.assertTrue(math.isnan(nb.verdict(5.0, 4.0, 0.0)["R"]))

    def test_real_over_io(self):
        self.assertAlmostEqual(nb.verdict(12.0, 10.0, 4.0)["real_over_io"], 1.2)
        self.assertTrue(math.isnan(nb.verdict(12.0, 0.0, 4.0)["real_over_io"]))

    def test_the_unit_does_not_matter(self):
        seconds = nb.verdict(10.0, 9.0, 4.0)
        nanoseconds = nb.verdict(10.0e9, 9.0e9, 4.0e9)
        self.assertEqual(seconds["critical_path"], nanoseconds["critical_path"])
        self.assertAlmostEqual(seconds["R"], nanoseconds["R"])


class SearcherJudgementTests(unittest.TestCase):
    def setUp(self):
        self.searcher, _ = searcher_for(lambda config, run: 0, {"x": 16})

    def test_a_gain_above_the_threshold_with_apart_ranges_is_better(self):
        best = nb.Stats([100.0, 100.0, 100.0])
        self.assertTrue(self.searcher.better(nb.Stats([96.0, 96.0, 96.0]), best))

    def test_a_gain_under_the_threshold_is_not_better(self):
        best = nb.Stats([100.0, 100.0, 100.0])
        self.assertFalse(self.searcher.better(nb.Stats([98.0, 98.0, 98.0]), best))
        self.assertFalse(self.searcher.better(nb.Stats([97.0, 97.0, 97.0]), best))
        self.assertTrue(self.searcher.better(nb.Stats([96.99, 96.99, 96.99]), best))

    def test_a_gain_whose_range_overlaps_the_best_is_not_better(self):
        best = nb.Stats([95.0, 100.0, 105.0])
        candidate = nb.Stats([91.0, 96.0, 101.0])
        self.assertLess(candidate.median, best.median * (1 - nb.GAIN_THRESHOLD))
        self.assertFalse(self.searcher.better(candidate, best))

    def test_a_slower_candidate_is_not_better(self):
        best = nb.Stats([100.0])
        self.assertFalse(self.searcher.better(nb.Stats([120.0]), best))

    def test_same_holds_within_the_threshold_of_the_best(self):
        best = nb.Stats([100.0, 100.0, 100.0])
        self.assertTrue(self.searcher.same(nb.Stats([103.0, 103.0, 103.0]), best))
        self.assertFalse(self.searcher.same(nb.Stats([104.0, 104.0, 104.0]), best))

    def test_same_holds_when_the_ranges_overlap(self):
        best = nb.Stats([95.0, 100.0, 105.0])
        self.assertTrue(self.searcher.same(nb.Stats([104.0, 130.0, 140.0]), best))

    def test_the_gain_threshold_is_three_percent(self):
        self.assertEqual(nb.GAIN_THRESHOLD, 0.03)


class LadderTests(unittest.TestCase):
    def ladder(self, start, direction, low=1, high=1 << 30):
        searcher, _ = searcher_for(lambda c, r: 0, {"x": start}, {"x": (low, high)})
        return list(searcher.ladder("x", start, direction))

    def test_it_doubles_up_for_the_steps_of_the_ladder(self):
        self.assertEqual(nb.LADDER_STEPS, 3)
        self.assertEqual(self.ladder(16, +1), [32, 64, 128])

    def test_it_halves_down(self):
        self.assertEqual(self.ladder(16, -1), [8, 4, 2])

    def test_it_stops_at_the_upper_bound(self):
        self.assertEqual(self.ladder(16, +1, high=40), [32])
        self.assertEqual(self.ladder(16, +1, high=16), [])

    def test_it_stops_at_the_lower_bound(self):
        self.assertEqual(self.ladder(16, -1, low=4), [8, 4])
        self.assertEqual(self.ladder(16, -1, low=16), [])

    def test_it_never_reaches_zero(self):
        self.assertEqual(self.ladder(1, -1, low=0), [])
        self.assertEqual(self.ladder(2, -1, low=0), [1])


class SearcherTests(unittest.TestCase):
    def test_a_knob_that_gains_at_each_step_is_pushed_up_the_ladder(self):
        searcher, evaluator = searcher_for(
            lambda config, run: 1000 * MS / config["x"], {"x": 4}
        )
        found = searcher.run(("x",), None)
        self.assertEqual(found, {"x": 32})
        # Doubled three times up; the first halving was worse and ended it.
        self.assertEqual({call[0]["x"] for call in evaluator.calls}, {2, 4, 8, 16, 32})

    def test_a_knob_that_gains_going_down_is_pushed_down(self):
        searcher, _ = searcher_for(lambda config, run: config["x"] * MS, {"x": 16})
        self.assertEqual(searcher.run(("x",), None), {"x": 2})

    def test_one_variable_moves_at_a_time(self):
        searcher, evaluator = searcher_for(
            lambda config, run: 1000 * MS / config.get("a", 4)
            + 1000 * MS / config.get("b", 4),
            {"a": 4, "b": 4},
        )
        found = searcher.run(("a", "b"), None)
        self.assertEqual(found, {"a": 32, "b": 32})
        for config, _ in evaluator.calls:
            # While a is searched, b is not in the configuration at all; while
            # b is searched, a keeps the value it was given.
            if "b" in config:
                self.assertEqual(config["a"], 32)
            else:
                self.assertEqual(set(config), {"a"})

    def test_only_what_moved_from_the_default_is_returned(self):
        searcher, _ = searcher_for(
            lambda config, run: 1000 * MS / config.get("a", 4) + 5 * MS,
            {"a": 4, "b": 4},
            # b does not matter: on its plateau, the default costs the least.
            memory_cost=lambda name, value: float(abs(value - 4)),
        )
        found = searcher.run(("a", "b"), None)
        self.assertEqual(found, {"a": 32})

    def test_a_gain_under_three_percent_is_not_kept(self):
        times = {16: 100.0, 32: 98.0, 64: 130.0, 8: 130.0, 4: 160.0, 128: 200.0, 2: 200.0}
        searcher, _ = searcher_for(lambda config, run: times[config["x"]] * MS, {"x": 16})
        self.assertEqual(searcher.run(("x",), None), {})

    def test_a_gain_whose_ranges_overlap_is_not_kept(self):
        # 4 % faster at the median, but the noise of the two overlap.
        noise = [-0.05, 0.0, 0.05, 0.0, 0.0]
        base = {16: 100.0, 32: 96.0, 8: 150.0, 64: 150.0}
        searcher, _ = searcher_for(
            lambda config, run: base.get(config["x"], 300.0) * MS * (1 + noise[run]),
            {"x": 16},
        )
        self.assertEqual(searcher.run(("x",), None), {})

    def test_the_same_gain_without_noise_is_kept(self):
        base = {16: 100.0, 32: 96.0, 8: 150.0, 64: 150.0}
        searcher, _ = searcher_for(
            lambda config, run: base.get(config["x"], 300.0) * MS, {"x": 16}
        )
        self.assertEqual(searcher.run(("x",), None), {"x": 32})

    def test_on_a_plateau_the_value_that_costs_the_least_memory_wins(self):
        searcher, _ = searcher_for(
            lambda config, run: (100 if config["x"] >= 8 else 200) * MS, {"x": 16}
        )
        # 8 to 128 are all as fast: the smallest is taken.
        self.assertEqual(searcher.run(("x",), None), {"x": 8})

    def test_the_memory_cost_is_the_callers_to_define(self):
        searcher, _ = searcher_for(
            lambda config, run: (100 if config["x"] >= 8 else 200) * MS,
            {"x": 16},
            memory_cost=lambda name, value: -float(value),
        )
        self.assertEqual(searcher.run(("x",), None), {"x": 128})

    def test_the_search_stays_inside_the_limits(self):
        searcher, evaluator = searcher_for(
            lambda config, run: 1000 * MS / config["x"],
            {"x": 4},
            {"x": (4, 16)},
        )
        self.assertEqual(searcher.run(("x",), None), {"x": 16})
        self.assertEqual({call[0]["x"] for call in evaluator.calls}, {4, 8, 16})

    def test_the_trace_tells_the_choice(self):
        searcher, _ = searcher_for(lambda config, run: 1000 * MS / config["x"], {"x": 4})
        searcher.run(("x",), None)
        self.assertTrue(any(line.startswith("x: 32 (from 4") for line in searcher.trace))

    def test_each_configuration_is_timed_three_times_then_five_when_it_matters(self):
        searcher, evaluator = searcher_for(
            lambda config, run: 1000 * MS / config["x"], {"x": 4}
        )
        searcher.run(("x",), None)
        runs_of = {}
        for config, index in evaluator.calls:
            runs_of.setdefault(config["x"], set()).add(index)
        self.assertEqual(runs_of[4], set(range(nb.CONFIRM_RUNS)))
        self.assertEqual(runs_of[2], set(range(nb.TRIAGE_RUNS)))
        self.assertEqual(nb.TRIAGE_RUNS, 3)
        self.assertEqual(nb.CONFIRM_RUNS, 5)

    def test_the_grid_takes_the_smallest_bytes_then_reads_of_the_plateau(self):
        def seconds(config, run):
            return (100 if config["reads"] >= 8 else 300) * MS

        searcher, _ = searcher_for(
            seconds, {"reads": 32, "bytes": 1 << 26}, {"reads": (1, 256), "bytes": (1, 1 << 40)}
        )
        chosen = searcher.tune_grid({}, "reads", "bytes", nb.GRID_READS, nb.GRID_BYTES)
        self.assertEqual(chosen, {"reads": 8, "bytes": 16 << 20})

    def test_a_grid_search_leads_the_run(self):
        def seconds(config, run):
            window = 1000 * MS / config.get("x", 4)
            return window + (100 if config.get("reads", 32) >= 8 else 300) * MS

        searcher, evaluator = searcher_for(
            seconds,
            {"x": 4, "reads": 32, "bytes": 1 << 26},
            {"x": (1, 1024), "reads": (1, 256), "bytes": (1, 1 << 40)},
        )
        found = searcher.run(("x",), ("reads", "bytes", nb.GRID_READS, nb.GRID_BYTES))
        self.assertEqual(found["reads"], 8)
        self.assertEqual(found["bytes"], 16 << 20)
        self.assertEqual(found["x"], 32)
        # The grid goes first: x is never timed without the grid's choice.
        tuned_x = [config for config, _ in evaluator.calls if "x" in config]
        self.assertTrue(all(config["reads"] == 8 for config in tuned_x))


class ResultsFileTests(unittest.TestCase):
    def test_the_encoding_does_not_depend_on_the_order_of_the_keys(self):
        self.assertEqual(
            nb.ResultsFile.encode({"b": 1, "a": 2}),
            nb.ResultsFile.encode({"a": 2, "b": 1}),
        )

    def test_a_result_is_found_by_configuration_and_run(self):
        results = nb.ResultsFile(None)
        results.add("M1", {"a": 1}, 0, 5.0)
        self.assertEqual(results.get({"a": 1}, 0), 5.0)
        self.assertIsNone(results.get({"a": 1}, 1))
        self.assertIsNone(results.get({"a": 2}, 0))

    def test_a_file_is_made_with_its_header_and_missing_folders(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "deeper" / "search.csv"
            nb.ResultsFile(path)
            with open(path, newline="") as handle:
                self.assertEqual(
                    list(csv.reader(handle)), [["pattern", "config", "run", "metric_ns"]]
                )

    def test_every_result_is_written_when_it_is_added(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "search.csv"
            results = nb.ResultsFile(path)
            results.add("M1", {"a": 1}, 0, 1234567.0)
            results.add("M2", {"a": 2}, 1, 7.0)
            with open(path, newline="") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(
                rows,
                [
                    {"pattern": "M1", "config": '{"a":1}', "run": "0", "metric_ns": "1234567"},
                    {"pattern": "M2", "config": '{"a":2}', "run": "1", "metric_ns": "7"},
                ],
            )

    def test_a_file_is_read_back(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "search.csv"
            nb.ResultsFile(path).add("M1", {"a": 1, "b": 2}, 2, 42.0)
            again = nb.ResultsFile(path)
            self.assertEqual(again.get({"b": 2, "a": 1}, 2), 42.0)


class ResumeTests(unittest.TestCase):
    @staticmethod
    def seconds(config, run):
        return 1000 * MS / config["x"] + 5 * MS

    def search(self, path):
        searcher, evaluator = searcher_for(
            self.seconds, {"x": 4}, results=nb.ResultsFile(path)
        )
        return searcher.run(("x",), None), evaluator

    def test_a_second_search_over_a_complete_file_measures_nothing(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "search.csv"
            first, evaluator = self.search(path)
            self.assertGreater(len(evaluator.calls), 0)
            second, again = self.search(path)
            self.assertEqual(again.calls, [])
            self.assertEqual(first, second)

    def test_a_partial_file_resumes_without_redoing_its_rows(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "search.csv"
            first, evaluator = self.search(path)
            total = len(evaluator.calls)
            lines = path.read_text().splitlines()
            kept = total // 2
            self.assertGreater(kept, 0)
            path.write_text("\n".join(lines[: 1 + kept]) + "\n")

            second, again = self.search(path)

            self.assertEqual(second, first)
            self.assertEqual(len(again.calls), total - kept)
            done = {
                (nb.ResultsFile.encode(config), run)
                for config, run in evaluator.calls[:kept]
            }
            for config, run in again.calls:
                self.assertNotIn((nb.ResultsFile.encode(config), run), done)
            # The file is whole again, with each run once.
            with open(path, newline="") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), total)
            self.assertEqual(
                len({(row["config"], row["run"]) for row in rows}), total
            )

    def test_the_rows_of_another_search_are_not_mixed_up(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "search.csv"
            self.search(path)
            other, evaluator = searcher_for(
                lambda config, run: config["y"] * MS,
                {"y": 8},
                results=nb.ResultsFile(path),
            )
            other.run(("y",), None)
            self.assertGreater(len(evaluator.calls), 0)


def busy_samples(rows, source="ioctl-disk-performance"):
    return {"source": source, "samples": rows}


def windows_row(t, query, idle):
    return {"t_unix_ns": t, "query_ns": query, "idle_ns": idle, "busy_ns": 0,
            "bytes_read": 0, "read_count": 0, "read_time_ns": 0, "queue_depth": 0}


def linux_row(t, busy):
    return {"t_unix_ns": t, "query_ns": 0, "idle_ns": 0, "busy_ns": busy,
            "bytes_read": 0, "read_count": 0, "read_time_ns": 0, "queue_depth": 0}


class BusySeriesTests(unittest.TestCase):
    def test_nothing_or_one_sample_gives_no_series(self):
        self.assertEqual(nb.busy_series(None), [])
        self.assertEqual(nb.busy_series({}), [])
        self.assertEqual(nb.busy_series(busy_samples([])), [])
        self.assertEqual(nb.busy_series(busy_samples([windows_row(0, 0, 0)])), [])

    def test_windows_busy_is_one_minus_the_idle_share_of_the_query_time(self):
        rows = [windows_row(0, 0, 0), windows_row(100 * MS, 100 * MS, 25 * MS)]
        series = nb.busy_series(busy_samples(rows))
        self.assertEqual(len(series), 1)
        start, end, busy = series[0]
        self.assertEqual((start, end), (0, 100 * MS))
        self.assertAlmostEqual(busy, 0.75)

    def test_windows_busy_uses_the_query_clock_not_the_wall_clock(self):
        # The query clock ran for 50 ms while 100 ms went by on the wall clock.
        rows = [windows_row(0, 0, 0), windows_row(100 * MS, 50 * MS, 25 * MS)]
        self.assertAlmostEqual(nb.busy_series(busy_samples(rows))[0][2], 0.5)

    def test_linux_busy_is_the_busy_time_over_the_wall_time(self):
        rows = [linux_row(0, 0), linux_row(100 * MS, 50 * MS)]
        series = nb.busy_series(busy_samples(rows, "proc-diskstats"))
        self.assertAlmostEqual(series[0][2], 0.5)

    def test_busy_is_clamped_between_zero_and_one(self):
        rows = [linux_row(0, 0), linux_row(100 * MS, 150 * MS), linux_row(200 * MS, 150 * MS - 1)]
        series = nb.busy_series(busy_samples(rows, "proc-diskstats"))
        self.assertEqual(series[0][2], 1.0)
        self.assertGreaterEqual(series[1][2], 0.0)
        rows = [windows_row(0, 0, 0), windows_row(100 * MS, 100 * MS, 200 * MS)]
        self.assertEqual(nb.busy_series(busy_samples(rows))[0][2], 0.0)

    def test_an_interval_that_goes_nowhere_is_skipped(self):
        rows = [
            windows_row(0, 0, 0),
            windows_row(0, 10 * MS, 0),
            windows_row(100 * MS, 10 * MS, 0),
            windows_row(200 * MS, 110 * MS, 50 * MS),
        ]
        series = nb.busy_series(busy_samples(rows))
        # The first pair has no wall time, the second no query time.
        self.assertEqual(len(series), 1)
        self.assertEqual(series[0][:2], (100 * MS, 200 * MS))

    def test_there_is_one_interval_between_two_samples(self):
        rows = [windows_row(index * 100 * MS, index * 100 * MS, 0) for index in range(6)]
        series = nb.busy_series(busy_samples(rows))
        self.assertEqual(len(series), 5)
        self.assertTrue(all(busy == 1.0 for _, _, busy in series))


def interval_rows(start_ns, busy_by_interval):
    """Windows samples every 100 ms with the given busy share in each interval."""
    rows = [windows_row(start_ns, 0, 0)]
    query = idle = 0
    for index, busy in enumerate(busy_by_interval):
        query += 100 * MS
        idle += round((1 - busy) * 100 * MS)
        rows.append(windows_row(start_ns + (index + 1) * 100 * MS, query, idle))
    return busy_samples(rows)


def run_summary(start_ns, phases):
    return {
        "run": {
            "start_unix_ns": start_ns,
            "phases": [
                {"name": name, "reads": reads, "first_begin_ns": begin, "last_end_ns": end}
                for name, reads, begin, end in phases
            ],
        }
    }


class PhaseBusyTests(unittest.TestCase):
    START = 5_000_000_000

    def samples(self):
        # Eleven intervals of 100 ms; the busy share of each.
        return interval_rows(
            self.START, [1.0, 1.0, 0.5, 0.5, 1.0, 0.0, 0.0, 0.5, 1.0, 1.0, 1.0]
        )

    def test_the_mean_is_weighted_by_the_time_of_each_interval(self):
        summary = run_summary(self.START, [("pass1", 10, 0, 400 * MS)])
        result = nb.phase_busy(summary, self.samples())
        # Intervals 0 to 3: busy 1, 1, 0.5, 0.5; two of four at the target.
        self.assertAlmostEqual(result["pass1"]["mean_busy"], 0.75)
        self.assertAlmostEqual(result["pass1"]["share_at_target"], 0.5)

    def test_an_interval_counts_for_the_part_that_lies_inside_the_phase(self):
        summary = run_summary(self.START, [("pass2", 10, 350 * MS, 550 * MS)])
        result = nb.phase_busy(summary, self.samples())
        # 50 ms of interval 3 (0.5), 100 ms of interval 4 (1.0), 50 ms of
        # interval 5 (0.0): (25 + 100 + 0) / 200.
        self.assertAlmostEqual(result["pass2"]["mean_busy"], 0.625)
        self.assertAlmostEqual(result["pass2"]["share_at_target"], 0.5)

    def test_the_target_is_ninety_five_percent(self):
        self.assertEqual(nb.BUSY_TARGET, 0.95)
        samples = interval_rows(self.START, [0.95, 0.949, 0.96, 0.9])
        summary = run_summary(self.START, [("pass1", 1, 0, 400 * MS)])
        result = nb.phase_busy(summary, samples)
        self.assertAlmostEqual(result["pass1"]["share_at_target"], 0.5)

    def test_a_phase_that_reads_nothing_is_left_out(self):
        summary = run_summary(
            self.START, [("link", 0, 0, 400 * MS), ("pass1", 3, 0, 400 * MS)]
        )
        self.assertEqual(list(nb.phase_busy(summary, self.samples())), ["pass1"])

    def test_a_phase_outside_the_samples_is_left_out(self):
        summary = run_summary(self.START, [("late", 3, 5000 * MS, 6000 * MS)])
        self.assertEqual(nb.phase_busy(summary, self.samples()), {})

    def test_a_phase_that_starts_before_the_samples_counts_what_they_cover(self):
        summary = run_summary(self.START, [("early", 3, -300 * MS, 200 * MS)])
        result = nb.phase_busy(summary, self.samples())
        self.assertAlmostEqual(result["early"]["mean_busy"], 1.0)

    def test_without_samples_there_is_nothing(self):
        summary = run_summary(self.START, [("pass1", 3, 0, 400 * MS)])
        self.assertEqual(nb.phase_busy(summary, None), {})
        self.assertEqual(nb.phase_busy(summary, busy_samples([])), {})

    def test_the_start_of_the_run_places_the_phases_on_the_samples(self):
        summary = run_summary(self.START + 200 * MS, [("pass1", 3, 0, 200 * MS)])
        result = nb.phase_busy(summary, self.samples())
        # The run began 200 ms into the samples: intervals 2 and 3, both 0.5.
        self.assertAlmostEqual(result["pass1"]["mean_busy"], 0.5)
        self.assertAlmostEqual(result["pass1"]["share_at_target"], 0.0)


class SmallFunctionTests(unittest.TestCase):
    def test_phase_wall(self):
        summary = {"run": {"phases": [{"name": "open", "wall_ns": 5}, {"name": "pass1", "wall_ns": 9}]}}
        self.assertEqual(nb.phase_wall(summary, "pass1"), 9)
        self.assertIsNone(nb.phase_wall(summary, "absent"))
        self.assertIsNone(nb.phase_wall(None, "pass1"))

    def test_bytes_read_during(self):
        rows = [dict(windows_row(0, 0, 0), bytes_read=100), dict(windows_row(1, 1, 0), bytes_read=700)]
        self.assertEqual(nb.bytes_read_during(busy_samples(rows)), 600)
        self.assertIsNone(nb.bytes_read_during(None))
        self.assertIsNone(nb.bytes_read_during(busy_samples(rows[:1])))

    def test_histogram_lines_skip_empty_buckets_and_scale_the_bars(self):
        lines = nb.histogram_lines({"buckets": [0, 4, 0, 2]}, width=8)
        self.assertEqual(len(lines), 2)
        self.assertIn("2 B+", lines[0])
        self.assertTrue(lines[0].endswith("#" * 8))
        self.assertIn("8 B+", lines[1])
        self.assertTrue(lines[1].endswith("#" * 4))
        self.assertEqual(nb.histogram_lines({"buckets": []}), [])

    def test_combo_key_names_the_combination(self):
        scenario = nb.Scenario("mftlist", "mftlist")
        self.assertEqual(
            nb.combo_key("c:", scenario, "deleted", ["--threads=2"]),
            "c:|mftlist|deleted|--threads=2",
        )

    def test_modes_pass_the_flags_of_the_tools(self):
        self.assertEqual(nb.MODES["none"], [])
        self.assertEqual(nb.MODES["recover"], ["--recover"])
        self.assertEqual(nb.MODES["deleted"], ["--deleted"])
        self.assertEqual(sorted(nb.MODES["both"]), ["--deleted", "--recover"])

    def test_the_search_targets_name_knobs_and_phases(self):
        self.assertEqual(sorted(nb.TARGETS), ["M1", "M2", "M3", "M4"])
        for pattern, target in nb.TARGETS.items():
            self.assertEqual(target.pattern, pattern)
            self.assertTrue(target.knobs)
            self.assertEqual(target.phase, target.stop_after)


def fabricated_results():
    sizes = {"count": 3, "buckets": [0, 2, 1]}
    run = {
        "wall_ns": 3_000_000_000,
        "reads": {"tail_ns": 100_000_000, "requested": 10, "requested_bytes": 5 << 20},
        "disk": {"bytes": 6 << 20, "amplification": 1.2, "wasted_bytes": 1 << 20,
                 "seeks": 4, "sizes": sizes, "queue_depth": [1, 3, 0, 1]},
        "phases": [
            {"name": "pass1", "wall_ns": 2_000_000_000, "cpu_ns": 1_500_000_000,
             "reads": 8, "read_bytes": 4 << 20, "read_wait_ns": 800_000_000,
             "first_begin_ns": 0, "last_end_ns": 2_000_000_000},
        ],
        "classes": {
            "reads": {"unclassified": 0, "io": 3, "window": 2, "ready": 1, "plan": 4},
            "wait_ns": {"unclassified": 0, "io": 3, "window": 0, "ready": 0, "plan": 4},
        },
        "threads": [{"role": "main", "user_ns": 1_000_000_000, "kernel_ns": 200_000_000}],
        "main_thread_ns": {"parse": 1_000_000_000, "read_wait": 800_000_000},
        "kinds": {"mft_window": {"reads": 10, "bytes": 5 << 20, "wait_ns": 800_000_000, "sizes": sizes}},
    }
    entry = {
        "volume": "c:", "scenario": "mftlist", "mode": "deleted", "extra": [],
        "real": [3.0e9, 3.1e9, 2.9e9], "io_recorded": [2.9e9, 3.0e9, 2.8e9],
        "io_ideal": [2.5e9, 2.6e9, 2.4e9], "cpu": [1.8e9, 1.9e9, 1.7e9],
        "listing_identical": True, "stray_disk_bytes": 100 << 20,
        "summary": {"run": run}, "busy": {"pass1": {"mean_busy": 0.9, "share_at_target": 0.5}},
    }
    return {
        "format": 1, "machine": "test machine",
        "combos": {"c:|mftlist|deleted|": entry},
        "disk_sheet": {"c:": {"zone": {"offset": 0, "length": 1 << 30},
                              "results": [{"pattern": "seq4m", "queue_depth": 1, "mb_per_s": 230.0,
                                           "iops": 55.0, "mean_latency_us": 1800.0}]}},
        "thread_sweep": {"c:|mftlist|deleted|2": [3.0e9, 2.9e9]},
    }


class ReportTests(unittest.TestCase):
    def test_a_report_gives_the_verdict_of_each_combination(self):
        text = nb.render_report(fabricated_results(), None)
        self.assertIn("# NTFS benchmark report", text)
        self.assertIn("Machine: test machine", text)
        self.assertIn("## Disk sheet of c:", text)
        self.assertIn("| c: | mftlist | deleted |", text)
        # T_real 3.0, T_io 2.9: the run is within 5 % of its I/O.
        self.assertIn("I/O (strong)", text)
        self.assertIn("same, 100 MiB read from the disk", text)
        self.assertIn("## Thread sweep", text)
        self.assertTrue(text.endswith("\n"))

    def test_a_combination_without_runs_is_left_out_of_the_table(self):
        results = fabricated_results()
        results["combos"]["c:|mftlist|deleted|"]["cpu"] = []
        text = nb.render_report(results, None)
        self.assertNotIn("| c: | mftlist | deleted |", text)

    def test_a_previous_run_gives_the_change(self):
        results = fabricated_results()
        previous = fabricated_results()
        previous["combos"]["c:|mftlist|deleted|"]["real"] = [6.0e9]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "results.json"
            path.write_text(json.dumps(previous))
            text = nb.render_report(results, str(path))
        self.assertIn("-50.0 %", text)

    def test_a_different_listing_is_flagged(self):
        results = fabricated_results()
        results["combos"]["c:|mftlist|deleted|"]["listing_identical"] = False
        self.assertIn("DIFFERENT", nb.render_report(results, None))


class ToolsTests(unittest.TestCase):
    def test_the_tool_is_found_in_a_folder_or_a_build_tree(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for config in ("Debug", "Release"):
                path = nb.exe(root / "tools" / config, "NtfsBench")
                path.parent.mkdir(parents=True)
                path.write_bytes(b"")
            tools = nb.Tools(root, "Release")
            self.assertEqual(tools.bench.parent.name, "Release")
            self.assertEqual(nb.Tools(root, "Debug").bench.parent.name, "Debug")
            direct = nb.exe(root, "NtfsBench")
            direct.write_bytes(b"")
            self.assertEqual(tools.bench, direct)

    def test_a_missing_tool_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(SystemExit):
                nb.Tools(Path(folder)).check()


class ParserTests(unittest.TestCase):
    def test_every_command_has_a_handler(self):
        parser = nb.build_parser()
        for argv in (
            ["audit", "--bin-dir", "b", "--out-dir", "o", "--volume", "c:"],
            ["search", "--bin-dir", "b", "--out-dir", "o", "--volume", "c:", "--profile-out", "p"],
            ["disk", "--bin-dir", "b", "--volume", "c:", "--out", "o"],
            ["report", "--results", "r", "--out", "o"],
        ):
            self.assertTrue(callable(parser.parse_args(argv).handler), argv)

    def test_the_lists_are_split_at_the_commas(self):
        args = nb.build_parser().parse_args(
            ["audit", "--bin-dir", "b", "--out-dir", "o", "--volume", "c:",
             "--modes", "none,both", "--threads", "1,2,8", "--tools", "mftlist"]
        )
        self.assertEqual(args.modes, ["none", "both"])
        self.assertEqual(args.threads, [1, 2, 8])
        self.assertEqual(args.tools, ["mftlist"])

    def test_a_missing_required_option_is_refused(self):
        with self.assertRaises(SystemExit) as raised:
            nb.build_parser().parse_args(["audit", "--bin-dir", "b"])
        self.assertEqual(raised.exception.code, 2)

    def test_no_command_prints_the_help_and_fails(self):
        shown = io.StringIO()
        with contextlib.redirect_stdout(shown):
            self.assertEqual(nb.main([]), 2)
        self.assertIn("usage:", shown.getvalue())


class SelfTestTests(unittest.TestCase):
    def require_bench(self):
        if not BENCH_EXE:
            self.skipTest("no NtfsBench given (--bench-exe or NTFS_BENCH_EXE)")

    def run_script(self, *arguments):
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), *arguments],
            capture_output=True, text=True, timeout=600, check=False,
        )

    def test_the_self_test_passes_on_the_analytic_disk(self):
        completed = self.run_script("--self-test")
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIn("self-test: passed", completed.stdout)
        self.assertNotIn("FAIL", completed.stdout)

    def test_the_self_test_passes_against_the_built_ntfsbench(self):
        self.require_bench()
        completed = self.run_script("--self-test", "--sim-exe", BENCH_EXE)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIn("self-test: passed", completed.stdout)
        self.assertIn("NtfsBench lists the knobs", completed.stdout)
        self.assertNotIn("FAIL", completed.stdout)

    def test_the_script_reads_the_knob_table_of_ntfsbench(self):
        self.require_bench()
        table = nb.load_knobs(nb.Tools(Path(BENCH_EXE).parent))
        self.assertEqual(len(table), 42)
        for target in nb.TARGETS.values():
            for name in target.knobs:
                self.assertIn(name, table, f"{target.pattern} searches {name}")
        for name in ("reader.max_reads", "reader.max_bytes"):
            self.assertIn(name, table)
        limits, defaults = nb.knob_limits(table, ["reader.max_reads@M1"], "M1")
        self.assertEqual(defaults["reader.max_reads@M1"], table["reader.max_reads"]["default"])
        self.assertEqual(limits["reader.max_reads@M1"][0], table["reader.max_reads"]["min"])

    def test_the_simulated_tool_gives_the_time_of_the_simulated_disk(self):
        self.require_bench()
        simulated = nb.SimulatedTool(Path(BENCH_EXE), "ssd")
        first = simulated.measure({}, 0)
        self.assertGreater(first, 0)
        self.assertEqual(first, simulated.measure({}, 1))


def repository_root() -> Path:
    for folder in (HERE, *HERE.parents):
        if (folder / "CMakePresets.json").exists():
            return folder
    raise unittest.SkipTest("the repository is not around the script")


def join_continuations(text: str) -> list[str]:
    """The lines of a PowerShell block, with the backtick continuations joined."""
    lines, current = [], ""
    for raw in text.splitlines():
        stripped = raw.strip()
        if stripped.endswith("`"):
            current += stripped[:-1].rstrip() + " "
            continue
        lines.append((current + stripped).strip())
        current = ""
    return lines


def split_words(command: str) -> list[str]:
    """Words of a command line, with the double quotes taken off."""
    return [word.strip('"') for word in re.findall(r'"[^"]*"|\S+', command)]


class WorkflowTests(unittest.TestCase):
    """.github/workflows/benchmark.yml agrees with the script and the project."""

    @classmethod
    def setUpClass(cls):
        cls.root = repository_root()
        cls.workflow = (cls.root / ".github" / "workflows" / "benchmark.yml").read_text()
        cls.env = dict(re.findall(r"^\s{6}(\w+): (\S+)\s*$", cls.workflow, re.M))

    def script_commands(self):
        commands = []
        for line in join_continuations(self.workflow):
            if "ntfs_benchmark.py" not in line:
                continue
            command = line[line.index("ntfs_benchmark.py") + len("ntfs_benchmark.py"):]
            command = re.sub(r"\$env:(\w+)", lambda m: self.env.get(m.group(1), "x"), command)
            command = re.sub(r"\$\{\{\s*inputs\.(\w+)\s*\}\}", "1", command)
            command = re.sub(r"\$\w+", "bench.exe", command)
            commands.append(split_words(command))
        return commands

    def test_the_workflow_runs_the_script_in_three_ways(self):
        commands = self.script_commands()
        self.assertEqual(
            [command[0] for command in commands], ["--self-test", "audit", "search"]
        )

    def test_the_script_the_workflow_runs_exists(self):
        self.assertIn("NTFSLibTests/tools/benchmark/ntfs_benchmark.py", self.workflow)
        self.assertTrue((self.root / "NTFSLibTests/tools/benchmark/ntfs_benchmark.py").exists())

    def test_every_command_line_is_accepted_by_the_parser_of_the_script(self):
        parser = nb.build_parser()
        for words in self.script_commands():
            with self.subTest(command=words[0]):
                try:
                    args = parser.parse_args(words)
                except SystemExit as error:
                    self.fail(f"the parser refuses {words}: exit {error.code}")
                if words[0] != "--self-test":
                    self.assertTrue(callable(args.handler))
                else:
                    self.assertTrue(args.self_test)
                    self.assertEqual(args.sim_exe, "bench.exe")

    def test_the_audit_and_the_search_use_values_the_script_knows(self):
        parser = nb.build_parser()
        by_command = {words[0]: parser.parse_args(words) for words in self.script_commands()
                      if words[0] != "--self-test"}
        audit = by_command["audit"]
        self.assertEqual(audit.volume, ["c:"])
        self.assertEqual(audit.threads, [1, 2])
        self.assertEqual(audit.rounds, 1)
        self.assertEqual(audit.caches, ["fullcache"])
        self.assertEqual(audit.dir_scenarios, ["subtree"])
        self.assertLessEqual(set(audit.modes), set(nb.MODES))
        self.assertLessEqual(set(audit.tools), {"mftlist", "dir"})
        search = by_command["search"]
        self.assertEqual(search.patterns, ["M1"])
        self.assertLessEqual(set(search.patterns), set(nb.TARGETS))
        self.assertEqual(search.volume, "c:")

    def test_the_scratch_option_names_a_folder_for_the_volume(self):
        audit = nb.build_parser().parse_args(
            next(words for words in self.script_commands() if words[0] == "audit")
        )
        runner = nb.Runner.__new__(nb.Runner)
        runner.args = audit
        runner.out_dir = Path(tempfile.gettempdir())
        mapping = dict(item.split("=", 1) for item in audit.scratch)
        self.assertEqual(list(mapping), ["c"])
        self.assertEqual(mapping["c"], "d:\\bench-scratch")

    def test_the_targets_it_builds_exist(self):
        built = re.search(r"--target\s+(.+)", self.workflow).group(1).split()
        declared = set()
        for path in (self.root / "NTFSLibTests").rglob("CMakeLists.txt"):
            declared.update(re.findall(r"add_executable\(\s*(\w+)", path.read_text()))
        for target in built:
            self.assertIn(target, declared, f"no add_executable({target})")
        for tool in ("NtfsMftList", "NtfsDir", "NtfsDir2", "NtfsBench"):
            self.assertIn(tool, built, f"{tool} is used by the script")

    def test_it_configures_a_preset_that_builds_where_it_looks(self):
        preset = re.search(r"cmake --preset (\S+)", self.workflow).group(1)
        presets = json.loads((self.root / "CMakePresets.json").read_text())
        configure = {entry["name"]: entry for entry in presets["configurePresets"]}
        self.assertIn(preset, configure)
        # A preset may inherit its binary directory.
        def binary_dir(name):
            entry = configure[name]
            if "binaryDir" in entry:
                return entry["binaryDir"]
            parents = entry.get("inherits", [])
            for parent in [parents] if isinstance(parents, str) else parents:
                found = binary_dir(parent)
                if found:
                    return found
            return None

        where = binary_dir(preset).replace("${sourceDir}/", "").replace("${presetName}", preset)
        self.assertEqual(where, self.env["BUILD"])

    def test_the_files_it_uploads_are_those_the_script_writes(self):
        source = SCRIPT.read_text()
        uploaded = re.findall(r"\$\{\{ env\.OUT \}\}/(\S+)", self.workflow)
        self.assertEqual(
            sorted(uploaded), sorted(["results.json", "report.md", "profile.json", "search-*.csv"])
        )
        self.assertIn('"results.json"', source)
        self.assertIn('"report.md"', source)
        self.assertIn('f"search-{pattern}.csv"', source)
        # profile.json is named by the workflow, as --profile-out.
        self.assertIn('--profile-out "$env:OUT/profile.json"', self.workflow)

    def test_it_runs_only_when_started_by_hand(self):
        self.assertRegex(self.workflow, r"(?m)^on:\n\s+workflow_dispatch:")
        self.assertNotRegex(self.workflow, r"(?m)^\s+(push|pull_request):")


def main() -> int:
    global BENCH_EXE
    arguments = sys.argv[1:]
    if "--bench-exe" in arguments:
        index = arguments.index("--bench-exe")
        BENCH_EXE = arguments[index + 1]
        del arguments[index : index + 2]
    unittest.main(argv=[sys.argv[0], *arguments], verbosity=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
