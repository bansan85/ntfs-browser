#!/usr/bin/env python3
"""Benchmark of NtfsMftList and ntfsdir: proves where the critical path lies, and
tunes the constants of the read path for one disk.

Standard library only, Python 3.12 or later. On Windows it needs an
administrator console (the volumes are opened raw); on Linux, read access to
the block device. See the README, section "Benchmark", for the method.

Commands:
  audit       T_real, T_io, T_cpu and R for each volume, tool and mode
  search      the constant search for one pattern; writes the profile
  disk        the sheet of the disk (NtfsBench diskbench)
  report      renders the Markdown report from the results of audit
  --self-test runs the searcher on a simulated disk with a known optimum
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import hashlib
import itertools
import json
import math
import os
import platform
import random
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Iterable

if sys.version_info < (3, 12):
    sys.exit("Python 3.12 or later is needed")

IS_WINDOWS = os.name == "nt"

# Flags of the four modes of a tool.
MODES: dict[str, list[str]] = {
    "none": [],
    "recover": ["--recover"],
    "deleted": ["--deleted"],
    "both": ["--deleted", "--recover"],
}

# A change is kept only if it gains more than this share of the time.
GAIN_THRESHOLD = 0.03
# Runs of a configuration in the triage, and in the confirmation.
TRIAGE_RUNS = 3
CONFIRM_RUNS = 5
# I/O is the critical path when the tool takes at most this much more than its
# replay: strongly up to the first, plainly up to the second.
CRITICAL_STRONG = 1.05
CRITICAL_PLAIN = 1.10
# Bytes the disk may read during a run on the captured image before the run
# counts as having touched the disk.
STRAY_READ_BYTES = 16 * 1024 * 1024
# A phase that reads should keep the disk this busy.
BUSY_TARGET = 0.95
# Seconds of the read ahead of the constant search, in steps of two.
LADDER_STEPS = 3
# A constant that has an effect only when another one is not 0: the search
# tries it after that one, and not at all while it stays 0.
KNOB_NEEDS = {"walk.read_ahead_bytes": "walk.read_ahead_dirs"}


# ------------------------------------------------------------------ helpers


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def exe(directory: Path, name: str) -> Path:
    return directory / (name + (".exe" if IS_WINDOWS else ""))


@dataclasses.dataclass
class Stats:
    """Median and range of a few runs."""

    values: list[float]

    @property
    def median(self) -> float:
        return statistics.median(self.values) if self.values else math.nan

    @property
    def low(self) -> float:
        return min(self.values) if self.values else math.nan

    @property
    def high(self) -> float:
        return max(self.values) if self.values else math.nan

    def text(self, scale: float = 1e9, unit: str = "s") -> str:
        if not self.values:
            return "n/a"
        return (
            f"{self.median / scale:.2f} {unit} "
            f"[{self.low / scale:.2f}-{self.high / scale:.2f}]"
        )


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while chunk := handle.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def allocated_ranges(path: Path) -> list[tuple[int, int]] | None:
    """(offset, length) of each range of a sparse file that holds data, in
    order, or None when the platform cannot say.

    A hole read through the file cache reserves its clusters on the drive that
    holds the file: reading the image of a 500 GB volume fills that drive, and
    the reads then fail with "no space left". A hole is therefore never read.
    """
    size = path.stat().st_size
    ranges: list[tuple[int, int]] = []
    if IS_WINDOWS:
        import ctypes
        import msvcrt
        from ctypes import wintypes

        fsctl_query_allocated_ranges = 0x000940CF
        error_more_data = 234
        batch = 4096

        class Range(ctypes.Structure):
            _fields_ = [("offset", ctypes.c_int64), ("length", ctypes.c_int64)]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.DeviceIoControl.argtypes = [
            wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
            ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
            ctypes.c_void_p,
        ]
        kernel32.DeviceIoControl.restype = wintypes.BOOL
        with open(path, "rb") as handle:
            os_handle = msvcrt.get_osfhandle(handle.fileno())
            cursor = 0
            while cursor < size:
                query = Range(cursor, size - cursor)
                answer = (Range * batch)()
                returned = wintypes.DWORD(0)
                ok = kernel32.DeviceIoControl(
                    os_handle, fsctl_query_allocated_ranges, ctypes.byref(query),
                    ctypes.sizeof(query), answer, ctypes.sizeof(answer),
                    ctypes.byref(returned), None,
                )
                if not ok and ctypes.get_last_error() != error_more_data:
                    return None
                count = returned.value // ctypes.sizeof(Range)
                if count == 0:
                    break
                ranges.extend((answer[i].offset, answer[i].length) for i in range(count))
                cursor = answer[count - 1].offset + answer[count - 1].length
        return ranges
    if not hasattr(os, "SEEK_DATA"):
        return None
    descriptor = os.open(path, os.O_RDONLY)
    try:
        cursor = 0
        while cursor < size:
            try:
                start = os.lseek(descriptor, cursor, os.SEEK_DATA)
            except OSError:
                break
            end = os.lseek(descriptor, start, os.SEEK_HOLE)
            ranges.append((start, end - start))
            cursor = end
    finally:
        os.close(descriptor)
    return ranges


def warm_file(path: Path) -> int:
    """Reads the data of a file through, to put it in the system file cache.

    Only the ranges that hold data: see allocated_ranges(). Returns the bytes read.
    """
    ranges = allocated_ranges(path)
    total = 0
    with open(path, "rb") as handle:
        if ranges is None:
            while chunk := handle.read(8 << 20):
                total += len(chunk)
            return total
        for offset, length in ranges:
            handle.seek(offset)
            while length > 0:
                chunk = handle.read(min(length, 8 << 20))
                if not chunk:
                    break
                length -= len(chunk)
                total += len(chunk)
    return total


def run_json(command: list[str], timeout: float | None = None) -> dict:
    """Runs a tool that prints JSON, and parses it."""
    completed = subprocess.run(
        command, capture_output=True, text=True, timeout=timeout, check=False
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{' '.join(command)} failed ({completed.returncode}): "
            f"{completed.stderr.strip()}"
        )
    return json.loads(completed.stdout)


# ------------------------------------------------------------------ the tools


@dataclasses.dataclass
class Tools:
    """Where the binaries are: a folder that holds them, or a build tree.

    With a build tree the Visual Studio generator puts each tool in a folder of
    its own, under a folder named after the configuration; `config` picks it.
    """

    directory: Path
    config: str = "Release"

    def find(self, name: str) -> Path:
        direct = exe(self.directory, name)
        if direct.exists():
            return direct
        found = sorted(self.directory.rglob(exe(Path(), name).name))
        preferred = [path for path in found if self.config in path.parts]
        return (preferred or found or [direct])[0]

    @property
    def mftlist(self) -> Path:
        return self.find("NtfsMftList")

    @property
    def dir_full(self) -> Path:
        return self.find("NtfsDir")

    @property
    def dir_none(self) -> Path:
        return self.find("NtfsDir2")

    @property
    def bench(self) -> Path:
        return self.find("NtfsBench")

    def check(self) -> None:
        for path in (self.mftlist, self.dir_full, self.bench):
            if not path.exists():
                raise SystemExit(f"{path} is missing: build the project first")


@dataclasses.dataclass(frozen=True)
class Scenario:
    """One thing a tool is asked to do, in any of the four modes."""

    name: str
    tool: str  # "mftlist", "dir" (FullCache) or "dir2" (NoCache)
    args: tuple[str, ...] = ()
    # Path inside the volume, for ntfsdir.
    subpath: str = ""
    # Phase whose wall time stands for the scenario in the constant search.
    phase: str = ""

    def target(self, volume: str) -> str:
        if self.tool == "mftlist":
            return volume
        if not self.subpath:
            return volume + ("\\" if IS_WINDOWS else "")
        sep = "\\" if IS_WINDOWS else "/"
        return volume.rstrip("\\/") + sep + self.subpath.replace("\\", sep)

    def executable(self, tools: Tools) -> Path:
        return {
            "mftlist": tools.mftlist,
            "dir": tools.dir_full,
            "dir2": tools.dir_none,
        }[self.tool]


def scenarios(args: argparse.Namespace) -> list[Scenario]:
    """The scenarios the command line selects."""
    chosen: list[Scenario] = []
    if "mftlist" in args.tools:
        chosen.append(Scenario("mftlist", "mftlist", phase="pass1"))
    for cache, tool in (("fullcache", "dir"), ("nocache", "dir2")):
        if cache not in args.caches or (tool == "dir2" and not IS_WINDOWS):
            continue
        if "dir" not in args.tools:
            continue
        if "full" in args.dir_scenarios:
            chosen.append(Scenario(f"dir-full-{cache}", tool, ("-r",), "", "walk"))
        if "subtree" in args.dir_scenarios:
            chosen.append(
                Scenario(f"dir-subtree-{cache}", tool, ("-r",), args.subtree, "walk")
            )
        if "bigdir" in args.dir_scenarios:
            chosen.append(
                Scenario(f"dir-bigdir-{cache}", tool, (), args.bigdir, "walk")
            )
        if "deep" in args.dir_scenarios:
            chosen.append(Scenario(f"dir-deep-{cache}", tool, (), args.deep, "walk"))
    return chosen


# ------------------------------------------------------------------ one run


@dataclasses.dataclass
class RunResult:
    wall_s: float
    exit_code: int
    summary: dict | None
    stdout: Path
    stderr: Path
    samples: dict | None


class Runner:
    """Starts the tools, cold or not, with the disk sampled meanwhile."""

    def __init__(self, tools: Tools, out_dir: Path, args: argparse.Namespace):
        self.tools = tools
        self.out_dir = out_dir
        self.args = args
        out_dir.mkdir(parents=True, exist_ok=True)

    # -- cold start
    def cold(self, volume: str) -> None:
        if self.args.no_cold:
            return
        purge = subprocess.run(
            [str(self.tools.bench), "purge"], capture_output=True, text=True
        )
        if purge.returncode != 0:
            raise SystemExit(
                purge.stderr.strip() or "cannot purge the file cache (use --no-cold)"
            )
        if not Path(volume).exists() or len(volume.rstrip("\\/:")) == 1:
            subprocess.run(
                [str(self.tools.bench), "wake", volume],
                capture_output=True,
                check=False,
            )

    # -- the scratch folder for the listing of a volume
    def scratch(self, volume: str) -> Path:
        mapping = dict(item.split("=", 1) for item in self.args.scratch)
        key = volume.rstrip("\\/:").lower()
        if key in mapping:
            path = Path(mapping[key])
        elif IS_WINDOWS and len(key) == 1:
            path = Path("E:\\tmp\\ntfs-benchmark" if key == "c" else "C:\\tmp\\ntfs-benchmark")
        else:
            path = self.out_dir / "scratch"
        path.mkdir(parents=True, exist_ok=True)
        return path

    # -- one process
    def run(
        self,
        command: list[str],
        label: str,
        sample_target: str | None,
        scratch: Path,
        timeout: float,
    ) -> RunResult:
        stdout = scratch / f"{label}.out"
        stderr = scratch / f"{label}.err"
        stop = scratch / f"{label}.stop"
        samples_path = scratch / f"{label}.samples.json"
        stop.unlink(missing_ok=True)
        sampler = None
        if sample_target and not self.args.no_sample:
            sampler = subprocess.Popen(
                [
                    str(self.tools.bench),
                    "sample",
                    sample_target,
                    f"--out={samples_path}",
                    f"--until-file={stop}",
                    "--interval-ms=100",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            time.sleep(0.3)
        began = time.perf_counter()
        with open(stdout, "wb") as out, open(stderr, "wb") as err:
            process = subprocess.Popen(command, stdout=out, stderr=err)
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                code = -9
        wall = time.perf_counter() - began
        samples = None
        if sampler is not None:
            stop.write_text("done")
            try:
                sampler.wait(timeout=10)
            except subprocess.TimeoutExpired:
                sampler.kill()
            if samples_path.exists():
                try:
                    samples = json.loads(samples_path.read_text())
                except json.JSONDecodeError:
                    samples = None
        return RunResult(wall, code, None, stdout, stderr, samples)

    def tool_command(
        self,
        scenario: Scenario,
        volume: str,
        mode: str,
        extra: list[str],
        summary: Path,
    ) -> list[str]:
        command = [str(scenario.executable(self.tools))]
        command += list(scenario.args)
        command += MODES[mode]
        command += extra
        command += [f"--benchmark={summary}"]
        command.append(scenario.target(volume))
        return command

    def run_tool(
        self,
        scenario: Scenario,
        volume: str,
        mode: str,
        label: str,
        extra: list[str],
        cold: bool,
        sample_target: str | None,
        timeout: float,
    ) -> RunResult:
        scratch = self.scratch(volume)
        summary = scratch / f"{label}.json"
        summary.unlink(missing_ok=True)
        if cold:
            self.cold(volume)
        result = self.run(
            self.tool_command(scenario, volume, mode, extra, summary),
            label,
            sample_target,
            scratch,
            timeout,
        )
        if summary.exists():
            try:
                result.summary = json.loads(summary.read_text())
            except json.JSONDecodeError:
                result.summary = None
        return result


# ------------------------------------------------------------------ analysis


def phase_wall(summary: dict | None, name: str) -> int | None:
    if not summary:
        return None
    for phase in summary["run"]["phases"]:
        if phase["name"] == name:
            return phase["wall_ns"]
    return None


def busy_series(samples: dict | None) -> list[tuple[int, int, float]]:
    """(start, end, busy share) of each interval between two samples, in Unix ns."""
    if not samples or len(samples.get("samples", [])) < 2:
        return []
    series = []
    rows = samples["samples"]
    for before, after in zip(rows, rows[1:]):
        span = after["t_unix_ns"] - before["t_unix_ns"]
        if span <= 0:
            continue
        if samples["source"] == "proc-diskstats":
            busy = (after["busy_ns"] - before["busy_ns"]) / span
        else:
            window = after["query_ns"] - before["query_ns"]
            if window <= 0:
                continue
            busy = 1.0 - (after["idle_ns"] - before["idle_ns"]) / window
        series.append((before["t_unix_ns"], after["t_unix_ns"], min(max(busy, 0.0), 1.0)))
    return series


def phase_busy(summary: dict, samples: dict | None) -> dict[str, dict]:
    """How busy the disk was during each phase that reads."""
    result: dict[str, dict] = {}
    series = busy_series(samples)
    if not series:
        return result
    start = summary["run"]["start_unix_ns"]
    for phase in summary["run"]["phases"]:
        if phase["reads"] == 0:
            continue
        begin = start + phase["first_begin_ns"]
        end = start + phase["last_end_ns"]
        inside = [
            (min(b, end) - max(a, begin), busy)
            for a, b, busy in series
            if b > begin and a < end
        ]
        total = sum(length for length, _ in inside)
        if total <= 0:
            continue
        mean = sum(length * busy for length, busy in inside) / total
        high = sum(length for length, busy in inside if busy >= BUSY_TARGET) / total
        result[phase["name"]] = {"mean_busy": mean, "share_at_target": high}
    return result


def bytes_read_during(samples: dict | None) -> int | None:
    if not samples or len(samples.get("samples", [])) < 2:
        return None
    rows = samples["samples"]
    return rows[-1]["bytes_read"] - rows[0]["bytes_read"]


def verdict(t_real: float, t_io: float, t_cpu: float) -> dict:
    """R and the critical path from the three times, in the same unit."""
    smaller = min(t_io, t_cpu)
    ratio = (t_io + t_cpu - t_real) / smaller if smaller > 0 else math.nan
    io_bound = t_cpu < t_io and t_real <= CRITICAL_PLAIN * t_io
    if io_bound and t_real <= CRITICAL_STRONG * t_io:
        path = "I/O (strong)"
    elif io_bound:
        path = "I/O"
    elif t_cpu >= t_io and t_real <= CRITICAL_PLAIN * t_cpu:
        path = "CPU"
    else:
        path = "mixed (neither floor explains the time)"
    return {"R": ratio, "critical_path": path, "real_over_io": t_real / t_io if t_io else math.nan}


# ------------------------------------------------------------------ audit


def combo_key(volume: str, scenario: Scenario, mode: str, extra: list[str]) -> str:
    return "|".join([volume, scenario.name, mode, " ".join(extra)])


def measure_combo(
    runner: Runner,
    scenario: Scenario,
    volume: str,
    mode: str,
    extra: list[str],
    rounds: int,
    done: dict,
    store: Callable[[], None],
) -> dict:
    """T_real, T_io and T_cpu of one combination, `rounds` times each."""
    key = combo_key(volume, scenario, mode, extra)
    entry = done.setdefault(
        key,
        {
            "volume": volume,
            "scenario": scenario.name,
            "mode": mode,
            "extra": extra,
            "real": [],
            "io_recorded": [],
            "io_ideal": [],
            "cpu": [],
            "listing_identical": None,
            "stray_disk_bytes": None,
            "summary": None,
            "busy": None,
        },
    )
    label_base = f"{scenario.name}-{mode}-{abs(hash(key)) % 10**8}"
    timeout = runner.args.timeout
    target_for_disk = None if Path(volume).is_file() else volume

    # T_real: the tool, cold.
    while len(entry["real"]) < rounds:
        index = len(entry["real"])
        log(f"  real  {index + 1}/{rounds}")
        result = runner.run_tool(
            scenario, volume, mode, f"{label_base}-real{index}", extra, True,
            target_for_disk, timeout,
        )
        if result.exit_code != 0 or result.summary is None:
            raise SystemExit(f"the run failed ({result.exit_code}): {result.stderr}")
        entry["real"].append(result.wall_s * 1e9)
        if index == 0:
            entry["summary"] = result.summary
            entry["real_listing_hash"] = sha256_of(result.stdout)
            entry["busy"] = phase_busy(result.summary, result.samples)
            entry["readlog"] = result.summary["read_log"]
            entry["readlog_kept"] = str(runner.scratch(volume) / f"{label_base}.readlog")
            shutil.copyfile(result.summary["read_log"], entry["readlog_kept"])
        store()

    # T_io: the read log, replayed through the reader, as recorded and with
    # every read announced up front.
    while len(entry["io_recorded"]) < rounds:
        log(f"  io    {len(entry['io_recorded']) + 1}/{rounds}")
        command = [
            str(runner.tools.bench), "replay", entry["readlog_kept"],
            volume, "--mode=both",
        ]
        if not runner.args.no_cold:
            command.append("--purge")
        replay = run_json(command, timeout=timeout)
        for run in replay["runs"]:
            key_name = "io_recorded" if run["mode"] == "recorded" else "io_ideal"
            entry[key_name].append(run["wall_ns"])
            if run["mode"] == "recorded" and not entry.get("replay_report"):
                entry["replay_report"] = run["report"]
        store()

    # T_cpu: capture the ranges the run read into an image on the scratch
    # drive, then run the tool on the image with the image in the file cache.
    image = runner.scratch(volume) / f"{label_base}.img"
    if not entry["cpu"]:
        log("  capture")
        capture = runner.run_tool(
            scenario, volume, mode, f"{label_base}-capture", [f"--capture={image}"] + extra,
            True, None, timeout,
        )
        if capture.exit_code != 0:
            raise SystemExit(f"the capture run failed: {capture.stderr}")
    image_scenario = scenario
    while len(entry["cpu"]) < rounds:
        index = len(entry["cpu"])
        log(f"  cpu   {index + 1}/{rounds}")
        warm_file(image)
        result = runner.run_tool(
            image_scenario, str(image), mode, f"{label_base}-cpu{index}", extra,
            False, target_for_disk, timeout,
        )
        if result.exit_code != 0 or result.summary is None:
            raise SystemExit(f"the run on the image failed: {result.stderr}")
        entry["cpu"].append(result.wall_s * 1e9)
        if index == 0:
            entry["listing_identical"] = (
                sha256_of(result.stdout) == entry.get("real_listing_hash")
            )
            entry["stray_disk_bytes"] = bytes_read_during(result.samples)
        store()
    image.unlink(missing_ok=True)
    return entry


def audit(args: argparse.Namespace) -> int:
    tools = Tools(Path(args.bin_dir), args.config)
    tools.check()
    out_dir = Path(args.out_dir)
    runner = Runner(tools, out_dir, args)
    results_path = out_dir / "results.json"
    done: dict = {}
    if results_path.exists() and not args.restart:
        done = json.loads(results_path.read_text()).get("combos", {})

    disk_sheet: dict = {}
    thread_sweep: dict = {}

    def store() -> None:
        results_path.write_text(
            json.dumps(
                {
                    "format": 1,
                    "machine": platform.platform(),
                    "combos": done,
                    "disk_sheet": disk_sheet,
                    "thread_sweep": thread_sweep,
                },
                indent=1,
            )
        )

    if results_path.exists() and not args.restart:
        previous = json.loads(results_path.read_text())
        disk_sheet.update(previous.get("disk_sheet", {}))
        thread_sweep.update(previous.get("thread_sweep", {}))

    for volume in args.volume:
        if volume not in disk_sheet and not args.no_disk_sheet:
            log(f"disk sheet of {volume}")
            try:
                disk_sheet[volume] = run_json(
                    [str(tools.bench), "diskbench", volume], timeout=args.timeout
                )
            except RuntimeError as error:
                log(f"  no sheet: {error}")
            store()
        for scenario in scenarios(args):
            for mode in args.modes:
                log(f"{volume} {scenario.name} {mode}")
                measure_combo(runner, scenario, volume, mode, [], args.rounds, done, store)

        if "mftlist" in args.tools and args.threads:
            for mode in args.modes:
                for count in args.threads:
                    key = f"{volume}|mftlist|{mode}|{count}"
                    entry = thread_sweep.setdefault(key, [])
                    scenario = Scenario("mftlist", "mftlist", phase="pass1")
                    while len(entry) < args.rounds:
                        log(f"{volume} mftlist {mode} --threads={count}")
                        result = runner.run_tool(
                            scenario, volume, mode, f"threads-{mode}-{count}",
                            [f"--threads={count}"], True, None, args.timeout,
                        )
                        entry.append(result.wall_s * 1e9)
                        store()

    store()
    report_path = out_dir / "report.md"
    report_path.write_text(render_report(json.loads(results_path.read_text()), args.previous))
    log(f"report: {report_path}")
    return 0


# ------------------------------------------------------------------ report


def histogram_lines(histogram: dict, width: int = 28) -> list[str]:
    buckets = histogram["buckets"]
    top = max(buckets) if buckets else 0
    lines = []
    for index, count in enumerate(buckets):
        if count == 0:
            continue
        bar = "#" * max(1, round(width * count / top)) if top else ""
        low = 1 << index if index else 0
        lines.append(f"{low:>12} B+ {count:>9}  {bar}")
    return lines


def render_report(results: dict, previous_path: str | None) -> str:
    previous = {}
    if previous_path and Path(previous_path).exists():
        previous = json.loads(Path(previous_path).read_text()).get("combos", {})
    lines = ["# NTFS benchmark report", "", f"Machine: {results.get('machine', '?')}", ""]

    sheets = results.get("disk_sheet", {})
    for volume, sheet in sheets.items():
        lines += [f"## Disk sheet of {volume}", ""]
        zone = sheet["zone"]
        lines += [
            f"Zone: offset {zone['offset']}, length {zone['length']} bytes.",
            "",
            "| Pattern | QD | MB/s | IOPS | mean latency (us) |",
            "|---|---:|---:|---:|---:|",
        ]
        for row in sheet["results"]:
            lines.append(
                f"| {row['pattern']} | {row['queue_depth']} | {row['mb_per_s']:.1f} | "
                f"{row['iops']:.0f} | {row['mean_latency_us']:.0f} |"
            )
        lines.append("")

    lines += [
        "## Verdict per volume, tool and mode",
        "",
        "I/O is critical when T_cpu < T_io and T_real <= 1.10 x T_io. "
        "R = (T_io + T_cpu - T_real) / min(T_io, T_cpu): 1 means the CPU work "
        "overlaps the I/O entirely, 0 means they run one after the other.",
        "",
        "| Volume | Scenario | Mode | T_real | T_io | T_io ideal | T_cpu | R | Critical path | Listing | vs previous |",
        "|---|---|---|---|---|---|---|---:|---|---|---|",
    ]
    for key, entry in results.get("combos", {}).items():
        real = Stats(entry["real"])
        recorded = Stats(entry["io_recorded"])
        ideal = Stats(entry["io_ideal"])
        cpu = Stats(entry["cpu"])
        if not (real.values and recorded.values and cpu.values):
            continue
        judged = verdict(real.median, recorded.median, cpu.median)
        listing = {True: "same", False: "DIFFERENT", None: "?"}[entry.get("listing_identical")]
        stray = entry.get("stray_disk_bytes")
        if stray is not None and stray > STRAY_READ_BYTES:
            listing += f", {stray >> 20} MiB read from the disk"
        change = ""
        if key in previous and previous[key]["real"]:
            before = statistics.median(previous[key]["real"])
            change = f"{(real.median / before - 1) * 100:+.1f} %"
        lines.append(
            f"| {entry['volume']} | {entry['scenario']} | {entry['mode']} | {real.text()} | "
            f"{recorded.text()} | {ideal.text()} | {cpu.text()} | {judged['R']:.2f} | "
            f"{judged['critical_path']} | {listing} | {change} |"
        )
    lines.append("")

    for key, entry in results.get("combos", {}).items():
        summary = entry.get("summary")
        if not summary:
            continue
        run = summary["run"]
        lines += [f"### {entry['volume']} {entry['scenario']} {entry['mode']}", ""]
        lines += [
            f"Wall {run['wall_ns'] / 1e9:.2f} s; serial tail after the last read "
            f"{run['reads']['tail_ns'] / 1e9:.2f} s; reads {run['reads']['requested']}, "
            f"{run['reads']['requested_bytes'] >> 20} MiB asked, "
            f"{run['disk']['bytes'] >> 20} MiB read by the disk "
            f"(amplification {run['disk']['amplification']:.2f}), "
            f"{run['disk']['wasted_bytes'] >> 20} MiB read ahead and wasted, "
            f"{run['disk']['seeks']} seeks.",
            "",
            "| Phase | wall (s) | CPU of its thread (s) | reads | MiB | waited (s) | disk busy | at 95 % |",
            "|---|---:|---:|---:|---:|---:|---:|---:|",
        ]
        busy = entry.get("busy") or {}
        for phase in run["phases"]:
            sampled = busy.get(phase["name"])
            lines.append(
                f"| {phase['name']} | {phase['wall_ns'] / 1e9:.2f} | {phase['cpu_ns'] / 1e9:.2f} | "
                f"{phase['reads']} | {phase['read_bytes'] >> 20} | {phase['read_wait_ns'] / 1e9:.2f} | "
                + (
                    f"{sampled['mean_busy'] * 100:.0f} % | {sampled['share_at_target'] * 100:.0f} % |"
                    if sampled
                    else "n/a | n/a |"
                )
            )
        lines += ["", "Classes of reads (the caller waited for the disk: io; the read was done and the window held others back: window; done and nothing held back: ready; not announced: plan):", ""]
        classes = run["classes"]
        lines += ["| class | reads | waited (s) |", "|---|---:|---:|"]
        for name, count in classes["reads"].items():
            lines.append(f"| {name} | {count} | {classes['wait_ns'][name] / 1e9:.2f} |")
        lines += ["", "CPU by thread:", "", "| thread | user (s) | kernel (s) |", "|---|---:|---:|"]
        for thread in run["threads"]:
            lines.append(
                f"| {thread['role']} | {thread['user_ns'] / 1e9:.2f} | {thread['kernel_ns'] / 1e9:.2f} |"
            )
        main = run["main_thread_ns"]
        lines += [
            "",
            "Main thread, exclusive: "
            + ", ".join(f"{name} {value / 1e9:.2f} s" for name, value in main.items()),
            "",
            "Read sizes by kind:",
            "",
        ]
        for name, kind in run["kinds"].items():
            lines.append(f"- {name}: {kind['reads']} reads, {kind['bytes'] >> 20} MiB, waited {kind['wait_ns'] / 1e9:.2f} s")
            lines += ["  ```"] + ["  " + line for line in histogram_lines(kind["sizes"])] + ["  ```"]
        lines += ["", "Size seen by the disk:", "", "```"] + histogram_lines(run["disk"]["sizes"]) + ["```", ""]
        depth = run["disk"]["queue_depth"]
        total = sum(depth) or 1
        lines += [
            "Queue depth when a read was issued: "
            + ", ".join(f"QD{index}: {count * 100 / total:.0f} %" for index, count in enumerate(depth) if count / total >= 0.01),
            "",
        ]

    sweep = results.get("thread_sweep", {})
    if sweep:
        lines += ["## Thread sweep (NtfsMftList --threads=N)", "", "| Volume | Mode | Threads | T_real |", "|---|---|---:|---|"]
        for key, values in sweep.items():
            volume, _, mode, count = key.split("|")
            lines.append(f"| {volume} | {mode} | {count} | {Stats(values).text()} |")
        lines.append("")
    return "\n".join(lines) + "\n"


def report(args: argparse.Namespace) -> int:
    results = json.loads(Path(args.results).read_text())
    Path(args.out).write_text(render_report(results, args.previous))
    log(f"report: {args.out}")
    return 0


# ------------------------------------------------------------------ disk sheet


def disk_command(args: argparse.Namespace) -> int:
    tools = Tools(Path(args.bin_dir), args.config)
    tools.check()
    sheet = run_json([str(tools.bench), "diskbench", args.volume], timeout=args.timeout)
    Path(args.out).write_text(json.dumps(sheet, indent=1))
    return 0


# ------------------------------------------------------------------ the search


@dataclasses.dataclass(frozen=True)
class Target:
    """One pattern to tune: how to time it, and the knobs that serve it."""

    pattern: str
    scenario: Scenario
    mode: str
    extra: tuple[str, ...]
    # Phase whose wall time is the measure, and where the run stops.
    phase: str
    stop_after: str
    # Knobs tried one at a time, in this order. The reader's caps are
    # "reader.<name>@<pattern>".
    knobs: tuple[str, ...]


TARGETS: dict[str, Target] = {
    "M1": Target(
        "M1", Scenario("mftlist", "mftlist"), "deleted", (), "pass1", "pass1",
        ("mft.window_bytes", "mft.max_gap_slots", "mft.max_announced_windows",
         "mft.max_announced_bytes", "mft.parse_chunk_slots"),
    ),
    "M2": Target(
        "M2", Scenario("mftlist", "mftlist"), "deleted", (), "pass2", "pass2",
        ("mft.max_announced_records", "mft.max_announced_ranges",
         "mft.announce_restart_interval", "mft.max_run_slots",
         "mft.max_preread_extensions", "mft.max_held_record_bytes",
         "mft.waiting_chunk_records"),
    ),
    # The recursive listing of a subtree: the read ahead of its directories
    # only acts when it recurses. search() gives the scenario the subtree.
    "M3": Target(
        "M3", Scenario("dir-subtree", "dir", ("-r",), "", "walk"), "none", (),
        "walk", "walk",
        ("walk.read_ahead_dirs", "walk.read_ahead_bytes", "cache.block_bytes"),
    ),
    "M4": Target(
        "M4", Scenario("mftlist", "mftlist"), "deleted", ("--verify-data=full",),
        "check", "check",
        ("check.max_read_bytes", "check.max_gap_bytes", "check.max_announced_reads",
         "check.max_announced_bytes"),
    ),
}

# Reads in flight and bytes in flight: searched on a grid, not apart.
GRID_READS = (4, 8, 16, 32, 64)
GRID_BYTES = tuple(mebibytes << 20 for mebibytes in (16, 32, 64, 128, 256))


class Evaluator:
    """Times a configuration: a dict of knob name to value."""

    def measure(self, config: dict[str, int], run_index: int) -> float:
        raise NotImplementedError


class ResultsFile:
    """The CSV of every run of the search, read back to resume it."""

    COLUMNS = ["pattern", "config", "run", "metric_ns"]

    def __init__(self, path: Path | None):
        self.path = path
        self.rows: dict[tuple[str, int], float] = {}
        if path and path.exists():
            with open(path, newline="") as handle:
                for row in csv.DictReader(handle):
                    self.rows[(row["config"], int(row["run"]))] = float(row["metric_ns"])
        elif path:
            path.parent.mkdir(parents=True, exist_ok=True)
            with open(path, "w", newline="") as handle:
                csv.writer(handle).writerow(self.COLUMNS)

    @staticmethod
    def encode(config: dict[str, int]) -> str:
        return json.dumps(config, sort_keys=True, separators=(",", ":"))

    def get(self, config: dict[str, int], run: int) -> float | None:
        return self.rows.get((self.encode(config), run))

    def add(self, pattern: str, config: dict[str, int], run: int, value: float) -> None:
        self.rows[(self.encode(config), run)] = value
        if self.path:
            with open(self.path, "a", newline="") as handle:
                csv.writer(handle).writerow([pattern, self.encode(config), run, f"{value:.0f}"])


def no_memory_cost(name: str, value: int) -> float:
    """The cost of a knob that is not given one: none, so a plateau keeps its value."""
    return 0.0


def memory_cost_of(table: dict[str, dict]) -> Callable[[str, int], float]:
    """The memory a value holds, in bytes, from the table of knobs of NtfsBench.

    `memory_unit_bytes` is what one unit of the value holds: 1 for a byte count,
    0 for a gap or a threshold. A name may carry a "@pattern" suffix.
    """

    def cost(name: str, value: int) -> float:
        return float(value) * table[name.split("@")[0]]["memory_unit_bytes"]

    return cost


class Searcher:
    """Finds the best value of each knob, one at a time, from the defaults.

    `memory_cost(name, value)` ranks the values of a plateau. A knob that holds
    no memory costs 0 for every value, and then a plateau keeps the value the
    search has at that point: the default, or the one chosen so far.
    """

    def __init__(self, evaluator: Evaluator, results: ResultsFile, pattern: str,
                 limits: dict[str, tuple[int, int]], defaults: dict[str, int],
                 memory_cost: Callable[[str, int], float] = no_memory_cost):
        self.evaluator = evaluator
        self.results = results
        self.pattern = pattern
        self.limits = limits
        self.defaults = defaults
        self.memory_cost = memory_cost
        self.trace: list[str] = []
        # What run() settled on, defaults included, and the names it searched.
        self.config: dict[str, int] = {}
        self.searched: set[str] = set()

    def runs(self, config: dict[str, int], count: int) -> Stats:
        values = []
        for index in range(count):
            known = self.results.get(config, index)
            if known is None:
                known = self.evaluator.measure(config, index)
                self.results.add(self.pattern, config, index, known)
            values.append(known)
        return Stats(values)

    def better(self, candidate: Stats, best: Stats) -> bool:
        """Gains more than the threshold, and the ranges do not overlap."""
        return (
            candidate.median < best.median * (1 - GAIN_THRESHOLD)
            and candidate.high < best.low
        )

    def regresses(self, candidate: Stats, best: Stats) -> bool:
        """Slower than the best by more than the threshold, at the median."""
        return candidate.median > best.median * (1 + GAIN_THRESHOLD)

    def same(self, candidate: Stats, best: Stats) -> bool:
        """Within the threshold of the best, or overlapping it: a plateau."""
        return (
            candidate.median <= best.median * (1 + GAIN_THRESHOLD)
            or candidate.low <= best.high
        )

    def ladder(self, name: str, start: int, direction: int) -> Iterable[int]:
        """The values tried from `start`, in steps of two, LADDER_STEPS at most.

        A knob that is off at 0 (its default) goes 0, 1, 2, 4 and back 4, 2, 1,
        0: the step between 0 and 1 is not a doubling and is not counted, since
        no doubling ever leaves 0.
        """
        low, high = self.limits[name]
        off_at_zero = self.defaults[name] == 0
        value = start
        steps = LADDER_STEPS
        while steps > 0:
            between_zero_and_one = False
            if direction > 0:
                if value == 0:
                    if not off_at_zero:
                        return
                    value, between_zero_and_one = 1, True
                else:
                    value *= 2
            elif value == 1 and off_at_zero:
                value, between_zero_and_one = 0, True
            else:
                value //= 2
            if value < low or value > high or (value == 0 and not between_zero_and_one):
                return
            yield value
            if not between_zero_and_one:
                steps -= 1

    def tune_knob(self, config: dict[str, int], name: str) -> dict[str, int]:
        current = dict(config)
        start = current.get(name, self.defaults[name])
        current[name] = start
        best = self.runs(current, TRIAGE_RUNS)
        plateau = [(start, best)]
        for direction in (+1, -1):
            value = start
            for candidate_value in self.ladder(name, start, direction):
                trial = dict(current)
                trial[name] = candidate_value
                stats = self.runs(trial, TRIAGE_RUNS)
                if self.better(stats, best) or self.same(stats, best):
                    # Confirm with more runs before it can win.
                    stats = self.runs(trial, CONFIRM_RUNS)
                    best_confirmed = self.runs(dict(current, **{name: value}), CONFIRM_RUNS)
                    self.trace.append(
                        f"{name}={candidate_value}: {stats.median / 1e6:.1f} ms vs "
                        f"{best_confirmed.median / 1e6:.1f} ms"
                    )
                if self.better(stats, best):
                    best = stats
                    value = candidate_value
                    plateau = [(candidate_value, stats)]
                elif self.same(stats, best):
                    plateau.append((candidate_value, stats))
                else:
                    break
        # On a plateau, the value that costs the least memory. min() takes the
        # first of equal costs, and the plateau lists the current value first.
        chosen = min(plateau, key=lambda item: self.memory_cost(name, item[0]))[0]
        self.trace.append(f"{name}: {chosen} (from {start}, plateau of {len(plateau)})")
        current[name] = chosen
        return current

    def tune_grid(self, config: dict[str, int], reads_name: str, bytes_name: str,
                  reads: Iterable[int], bytes_: Iterable[int]) -> dict[str, int]:
        best: tuple[Stats, dict[str, int]] | None = None
        plateau: list[tuple[Stats, dict[str, int]]] = []
        for count, size in itertools.product(reads, bytes_):
            trial = dict(config)
            trial[reads_name] = count
            trial[bytes_name] = size
            stats = self.runs(trial, TRIAGE_RUNS)
            if best is None or self.better(stats, best[0]):
                best = (stats, trial)
                plateau = [best]
            elif self.same(stats, best[0]):
                plateau.append((stats, trial))
        assert best is not None
        chosen = min(
            plateau,
            key=lambda item: (item[1][bytes_name], item[1][reads_name]),
        )[1]
        self.trace.append(
            f"grid {reads_name} x {bytes_name}: {chosen[reads_name]} reads, "
            f"{chosen[bytes_name] >> 20} MiB ({len(plateau)} on the plateau)"
        )
        return chosen

    def run(self, knobs: Iterable[str], grid: tuple[str, str, Iterable[int], Iterable[int]] | None) -> dict[str, int]:
        config: dict[str, int] = {}
        if grid:
            config = self.tune_grid(config, *grid)
            self.searched.update(grid[:2])
        for name in knobs:
            # A constant that only matters when another is on is left alone
            # while that one is off: its plateau would be the least memory.
            parent = KNOB_NEEDS.get(name)
            if parent is not None and config.get(parent, self.defaults.get(parent, 0)) == 0:
                continue
            config = self.tune_knob(config, name)
            self.searched.add(name)
        self.config = config
        # Only what moved away from the default goes in the profile.
        return {name: value for name, value in config.items() if value != self.defaults.get(name)}


class ToolEvaluator(Evaluator):
    """Times the real tool on a real volume, cold, with --stop-after."""

    def __init__(self, runner: Runner, target: Target, volume: str, pattern: str):
        self.runner = runner
        self.target = target
        self.volume = volume
        self.pattern = pattern

    def extra_for(self, config: dict[str, int]) -> list[str]:
        extra = [f"--stop-after={self.target.stop_after}", *self.target.extra]
        for name, value in sorted(config.items()):
            suffix = f"@{self.pattern}" if name.startswith("reader.") else ""
            extra.append(f"--tune={name}{suffix}={value}")
        return extra

    def measure(self, config: dict[str, int], run_index: int) -> float:
        log(f"  {json.dumps(config, sort_keys=True)} run {run_index}")
        result = self.runner.run_tool(
            self.target.scenario, self.volume, self.target.mode,
            f"search-{self.pattern}", self.extra_for(config), True, None,
            self.runner.args.timeout,
        )
        wall = phase_wall(result.summary, self.target.phase)
        if result.exit_code != 0 or wall is None:
            raise SystemExit(f"the run failed ({result.exit_code}): {result.stderr}")
        return float(wall)


def target_of(pattern: str, subtree: str) -> Target:
    """The target of a pattern. M3 lists `subtree` of the volume recursively."""
    target = TARGETS[pattern]
    if pattern == "M3":
        scenario = dataclasses.replace(target.scenario, subpath=subtree)
        target = dataclasses.replace(target, scenario=scenario)
    return target


def load_knobs(tools: Tools) -> dict[str, dict]:
    table = run_json([str(tools.bench), "knobs"])["knobs"]
    return {knob["name"]: knob for knob in table}


def knob_limits(table: dict[str, dict], names: Iterable[str], pattern: str) -> tuple[dict, dict]:
    limits, defaults = {}, {}
    for name in names:
        base = name.split("@")[0]
        limits[name] = (table[base]["min"], table[base]["max"])
        defaults[name] = table[base]["default"]
    return limits, defaults


def search(args: argparse.Namespace) -> int:
    tools = Tools(Path(args.bin_dir), args.config)
    tools.check()
    out_dir = Path(args.out_dir)
    runner = Runner(tools, out_dir, args)
    table = load_knobs(tools)
    profile_path = Path(args.profile_out)
    disk = run_json([str(tools.bench), "disk-id", args.volume])
    key = disk.get("key") or "*"
    profile = {"format": 1, "generator": "ntfs_benchmark.py", "disks": {}}
    if profile_path.exists():
        profile = json.loads(profile_path.read_text())
    section = profile["disks"].setdefault(key, {})
    section["label"] = args.label or args.volume
    if disk.get("solid_state") is not None:
        section["solid_state"] = disk["solid_state"]

    cost = memory_cost_of(table)
    searchers: dict[str, Searcher] = {}
    found: dict[str, dict[str, int]] = {}
    for pattern in args.patterns:
        target = target_of(pattern, args.subtree)
        names = list(target.knobs) + ["reader.max_reads", "reader.max_bytes"]
        limits, defaults = knob_limits(table, names, pattern)
        results = ResultsFile(out_dir / f"search-{pattern}.csv")
        searcher = Searcher(ToolEvaluator(runner, target, args.volume, pattern), results,
                            pattern, limits, defaults, cost)
        log(f"searching {pattern}")
        found[pattern] = searcher.run(
            target.knobs, ("reader.max_reads", "reader.max_bytes", GRID_READS, GRID_BYTES)
        )
        searchers[pattern] = searcher
        for line in searcher.trace:
            log("  " + line)

    # One common value when no pattern regresses by more than the threshold
    # with it, else a section for each pattern.
    found = settle_common(searchers, found, cost)
    section_values = split_profile(found)
    section.update(section_values)
    profile_path.write_text(json.dumps(profile, indent=2))
    log(f"profile: {profile_path}")

    if args.validate:
        return validate(args, runner, profile_path)
    return 0


def settle_common(
    searchers: dict[str, Searcher],
    found: dict[str, dict[str, int]],
    memory_cost: Callable[[str, int], float],
) -> dict[str, dict[str, int]]:
    """Gives a constant that several patterns searched one value, when it pays.

    A value is common when no pattern that searched the constant runs more than
    GAIN_THRESHOLD slower with it than with the value it found for itself, the
    rest of its configuration kept. The candidates are the values the patterns
    found, and the default. Of those that pass, the one that holds the least
    memory wins, and the default wins a tie. A configuration the search never
    timed is timed now, through the results file, so a resumed search does not
    repeat it. When no candidate passes, the patterns keep their own values.

    Returns `found` with the common values written into every pattern that
    searched the constant (or removed from it, when the common value is the
    default). `split_profile` then groups them.
    """
    settled = {pattern: dict(config) for pattern, config in found.items()}
    # Each pattern's own best is the baseline; the working configuration takes
    # the common values as they are settled, so that the last check is made on
    # what the profile will hold.
    working = {pattern: dict(searcher.config) for pattern, searcher in searchers.items()}
    baseline: dict[str, Stats] = {}
    counts: dict[str, int] = {}
    for searcher in searchers.values():
        for name in searcher.searched:
            counts[name] = counts.get(name, 0) + 1
    for name in sorted(name for name, count in counts.items() if count > 1):
        users = [pattern for pattern, searcher in searchers.items() if name in searcher.searched]
        default = searchers[users[0]].defaults[name]
        candidates = {working[pattern].get(name, default) for pattern in users} | {default}
        ordered = sorted(candidates, key=lambda v: (memory_cost(name, v), v != default, v))
        for value in ordered:
            passes = True
            for pattern in users:
                searcher = searchers[pattern]
                if pattern not in baseline:
                    baseline[pattern] = searcher.runs(searcher.config, TRIAGE_RUNS)
                trial = searcher.runs(dict(working[pattern], **{name: value}), TRIAGE_RUNS)
                if searcher.regresses(trial, baseline[pattern]):
                    passes = False
                    break
            if not passes:
                continue
            for pattern in users:
                working[pattern][name] = value
                if value == default:
                    settled[pattern].pop(name, None)
                else:
                    settled[pattern][name] = value
            log(f"  {name}: {value} for {', '.join(users)}")
            break
        else:
            log(f"  {name}: one value for each pattern")
    return settled


def split_profile(found: dict[str, dict[str, int]]) -> dict[str, dict[str, int]]:
    """Moves a value into 'common' when every searched pattern holds it, equal.

    settle_common() makes the patterns agree when the 3 % rule allows it.
    """
    section: dict[str, dict[str, int]] = {}
    names = {name for config in found.values() for name in config}
    for name in sorted(names):
        values = {pattern: config[name] for pattern, config in found.items() if name in config}
        # A pattern that was searched and left the knob at its default has no
        # value, and then the others' value is not common to all.
        everywhere = len(values) == len(found) and len(found) > 1
        if everywhere and len(set(values.values())) == 1:
            section.setdefault("common", {})[name] = next(iter(values.values()))
        else:
            for pattern, value in values.items():
                section.setdefault(pattern, {})[name] = value
    return section


def validate(args: argparse.Namespace, runner: Runner, profile_path: Path) -> int:
    """The full tool in its four modes, with and without the profile."""
    status = 0
    for mode in MODES:
        scenario = Scenario("mftlist", "mftlist")
        times: dict[str, Stats] = {}
        hashes = {}
        for label, extra in (("default", []), ("profile", [f"--profile={profile_path}"])):
            values = []
            for index in range(TRIAGE_RUNS):
                result = runner.run_tool(scenario, args.volume, mode, f"validate-{label}",
                                         extra, True, None, args.timeout)
                if result.exit_code != 0:
                    raise SystemExit(f"the validation run failed: {result.stderr}")
                values.append(result.wall_s * 1e9)
                hashes.setdefault(label, sha256_of(result.stdout))
            times[label] = Stats(values)
        change = times["profile"].median / times["default"].median - 1
        same = hashes["default"] == hashes["profile"]
        log(f"validate {mode}: default {times['default'].text()}, profile "
            f"{times['profile'].text()} ({change * 100:+.1f} %), listing "
            f"{'same' if same else 'DIFFERENT'}")
        if change > GAIN_THRESHOLD or not same:
            status = 1
    return status


# ------------------------------------------------------------------ self test


class AnalyticDisk(Evaluator):
    """A disk whose time follows a formula, with a known optimum.

    The scan reads TOTAL bytes in windows of window_bytes. A read takes a
    latency and its bytes at a lane's speed; max_reads reads overlap, on the
    lanes the disk has, and bytes in flight are capped by max_bytes. The CPU
    parses a window while the next ones are read. Noise of up to 1 % is added,
    reproducibly.
    """

    TOTAL = 4 << 30
    LATENCY = 0.0001
    LANE_RATE = 130e6
    LANES = 4
    CPU_RATE = 700e6

    def __init__(self, seed: int = 7):
        self.seed = seed

    def seconds(self, config: dict[str, int]) -> float:
        window = config.get("mft.window_bytes", 4 << 20)
        reads = config.get("reader.max_reads", 32)
        capacity = config.get("reader.max_bytes", 64 << 20)
        in_flight = max(1, min(reads, capacity // window, self.LANES))
        windows = math.ceil(self.TOTAL / window)
        io = windows * (self.LATENCY + window / self.LANE_RATE) / in_flight
        cpu = self.TOTAL / self.CPU_RATE
        # A wide gap costs reads of bytes nobody parses; none at all cuts a
        # window into many reads.
        gap = config.get("mft.max_gap_slots", 64)
        waste = 1 + (0.0002 * gap) + 0.5 / (1 + gap)
        return max(io * waste, cpu) + 0.2

    def measure(self, config: dict[str, int], run_index: int) -> float:
        # Not hash(): the hash of a string changes from one process to the next.
        text = f"{self.seed}|{json.dumps(config, sort_keys=True)}|{run_index}"
        rng = random.Random(hashlib.sha256(text.encode()).digest())
        return self.seconds(config) * 1e9 * (1 + rng.uniform(-0.01, 0.01))


class SimulatedTool(Evaluator):
    """The simulated disk of NtfsBench: a scan of a synthetic volume."""

    def __init__(self, bench: Path, model: str):
        self.bench = bench
        self.model = model

    def measure(self, config: dict[str, int], run_index: int) -> float:
        command = [str(self.bench), "simulate", f"--model={self.model}",
                   "--consume-ns-per-mib=1000000"]
        for name, value in sorted(config.items()):
            command.append(f"--tune={name}={value}")
        return float(run_json(command)["sim_ns"])


def self_test(args: argparse.Namespace) -> int:
    failures = 0

    def check(condition: bool, message: str) -> None:
        nonlocal failures
        print(("ok   " if condition else "FAIL ") + message)
        if not condition:
            failures += 1

    # 1. The statistics and the verdict.
    check(Stats([3, 1, 2]).median == 2, "median of three runs")
    judged = verdict(10.0, 9.8, 4.0)
    check(judged["critical_path"].startswith("I/O"), "I/O-bound run is called I/O")
    judged = verdict(14.0, 5.0, 9.0)
    check(judged["R"] == 0.0, "R is 0 when the phases run in sequence")
    judged = verdict(9.0, 9.0, 4.0)
    check(abs(judged["R"] - 1.0) < 1e-9, "R is 1 when the CPU hides under the I/O")

    # 2. The search finds the optimum of a known disk.
    limits = {
        "mft.window_bytes": (4096, 1 << 30),
        "mft.max_gap_slots": (0, 1 << 24),
        "reader.max_reads": (1, 256),
        "reader.max_bytes": (4096, 1 << 40),
    }
    defaults = {"mft.window_bytes": 4 << 20, "mft.max_gap_slots": 64,
                "reader.max_reads": 32, "reader.max_bytes": 64 << 20}
    disk = AnalyticDisk()
    # Bytes cost memory; a gap in slots costs none and keeps its value.
    memory = {"mft.window_bytes": 1, "mft.max_gap_slots": 0, "reader.max_reads": 1 << 20,
              "reader.max_bytes": 1}
    cost = lambda name, value: float(value) * memory[name]  # noqa: E731
    searcher = Searcher(disk, ResultsFile(None), "M1", limits, defaults, cost)
    found = searcher.run(("mft.window_bytes", "mft.max_gap_slots"),
                         ("reader.max_reads", "reader.max_bytes", GRID_READS, GRID_BYTES))
    tuned = dict(defaults, **found)
    candidates = itertools.product(
        (1 << shift for shift in range(14, 27)), (0, 16, 64, 256), GRID_READS, GRID_BYTES
    )
    best_seconds = min(
        disk.seconds({"mft.window_bytes": w, "mft.max_gap_slots": g,
                      "reader.max_reads": r, "reader.max_bytes": b})
        for w, g, r, b in candidates
    )
    got = disk.seconds(tuned)
    check(got <= best_seconds * (1 + GAIN_THRESHOLD),
          f"the search is within {GAIN_THRESHOLD:.0%} of the best of the grid "
          f"({got:.2f} s against {best_seconds:.2f} s, {found})")
    start = disk.seconds(defaults)
    check(got <= start * (1 + GAIN_THRESHOLD),
          f"the search does not make the default worse by more than the threshold "
          f"({start:.2f} s to {got:.2f} s)")

    # 3. It takes the least memory on a plateau: more bytes in flight than the
    #    disk can use is not chosen.
    check(tuned["reader.max_bytes"] <= defaults["reader.max_bytes"] * 2,
          "a plateau does not cost more memory than the disk can use")

    # 4. Resuming skips the runs the CSV holds.
    calls = []

    class Counting(Evaluator):
        def measure(self, config, run_index):
            calls.append((json.dumps(config, sort_keys=True), run_index))
            return disk.measure(config, run_index)

    results = ResultsFile(None)
    Searcher(Counting(), results, "M1", limits, defaults).run(("mft.window_bytes",), None)
    first = len(calls)
    Searcher(Counting(), results, "M1", limits, defaults).run(("mft.window_bytes",), None)
    check(first > 0 and len(calls) == first, "a second search with the same results measures nothing")

    # 4b. A constant that costs no memory keeps its value on a plateau.
    class Flat(Evaluator):
        def measure(self, config, run_index):
            return 1e9

    flat = Searcher(Flat(), ResultsFile(None), "M1", limits, defaults, cost)
    kept = flat.tune_knob({}, "mft.max_gap_slots")
    check(kept["mft.max_gap_slots"] == defaults["mft.max_gap_slots"],
          "a gap that costs no memory keeps its value on a flat plateau "
          f"({kept['mft.max_gap_slots']})")

    # 4b2. A constant that is off at 0 steps 1, 2, 4 up from 0, and 0 below 1.
    class Staircase(Evaluator):
        """More directories read ahead is faster up to 8; 0 is fastest from 1."""

        def __init__(self, seconds: dict[int, float]):
            self.seconds = seconds

        def measure(self, config, run_index):
            return self.seconds.get(config.get("walk.read_ahead_dirs", 0), 10.0) * 1e9

    off_limits = {"walk.read_ahead_dirs": (0, 4096), "walk.read_ahead_bytes": (65536, 1 << 30)}
    off_defaults = {"walk.read_ahead_dirs": 0, "walk.read_ahead_bytes": 16 << 20}
    stairs = Staircase({0: 10.0, 1: 9.0, 2: 7.0, 4: 5.0, 8: 4.0, 16: 4.0})
    check(list(Searcher(stairs, ResultsFile(None), "M3", off_limits, off_defaults)
               .ladder("walk.read_ahead_dirs", 0, +1)) == [1, 2, 4, 8],
          "a constant that is off at 0 goes up 1, 2, 4, 8")
    check(list(Searcher(stairs, ResultsFile(None), "M3", off_limits, off_defaults)
               .ladder("walk.read_ahead_dirs", 4, -1)) == [2, 1, 0],
          "a constant that is off at 0 goes down to 0 below 1")
    check(list(Searcher(stairs, ResultsFile(None), "M3", limits, defaults)
               .ladder("mft.max_gap_slots", 1, -1)) == [],
          "a constant that is not off at 0 never goes down to 0")
    stepped = Searcher(stairs, ResultsFile(None), "M3", off_limits, off_defaults)
    found = stepped.run(("walk.read_ahead_dirs", "walk.read_ahead_bytes"), None)
    check(found.get("walk.read_ahead_dirs") == 8,
          f"the search leaves 0 for the best window ({found})")
    check("walk.read_ahead_bytes" in stepped.searched,
          "the budget of the read ahead is searched once the read ahead is on")
    worse = Staircase({0: 1.0, 1: 5.0, 2: 6.0, 4: 7.0, 8: 8.0})
    down = Searcher(worse, ResultsFile(None), "M3", off_limits, off_defaults)
    kept_off = down.tune_knob({"walk.read_ahead_dirs": 1}, "walk.read_ahead_dirs")
    check(kept_off["walk.read_ahead_dirs"] == 0,
          "the search goes down from 1 to 0 when the read ahead does not pay")
    nothing = Searcher(worse, ResultsFile(None), "M3", off_limits, off_defaults)
    found = nothing.run(("walk.read_ahead_dirs", "walk.read_ahead_bytes"), None)
    check(found == {} and "walk.read_ahead_bytes" not in nothing.searched,
          "the budget is not searched while the read ahead stays off")

    # 4c. One value for a constant two patterns searched, when neither loses 3 %.
    pair = {name: Searcher(AnalyticDisk(seed), ResultsFile(None), name, limits, defaults, cost)
            for name, seed in (("M1", 7), ("M2", 11))}
    own = {name: searcher.run((), ("reader.max_reads", "reader.max_bytes", GRID_READS, GRID_BYTES))
           for name, searcher in pair.items()}
    merged = split_profile(settle_common(pair, own, cost))
    for name, searcher in pair.items():
        config = dict(defaults, **merged.get("common", {}), **merged.get(name, {}))
        own_best = searcher.evaluator.seconds(dict(defaults, **own[name]))
        check(searcher.evaluator.seconds(config) <= own_best * (1 + GAIN_THRESHOLD),
              f"the common values cost {name} less than {GAIN_THRESHOLD:.0%} of its own best")

    # 5. The profile split puts a value that every pattern agrees on in common.
    section = split_profile({"M1": {"reader.max_reads": 16, "mft.window_bytes": 1 << 20},
                             "M2": {"reader.max_reads": 16}})
    check(section == {"common": {"reader.max_reads": 16}, "M1": {"mft.window_bytes": 1 << 20}},
          "a value shared by all patterns goes in the common section")
    section = split_profile({"M1": {"reader.max_reads": 16}, "M2": {"reader.max_reads": 8}})
    check(section == {"M1": {"reader.max_reads": 16}, "M2": {"reader.max_reads": 8}},
          "a value that differs by pattern gets a section for each")

    # 6. The simulated disk of NtfsBench, when the binary is given.
    if args.sim_exe:
        bench = Path(args.sim_exe)
        sim = SimulatedTool(bench, "hdd")
        sim_limits = {"mft.window_bytes": (4096, 1 << 30), "reader.max_reads": (1, 256)}
        sim_defaults = {"mft.window_bytes": 4 << 20, "reader.max_reads": 32}
        small = {"mft.window_bytes": 4096}
        big = {"mft.window_bytes": 1 << 20}
        check(sim.measure(small, 0) > sim.measure(big, 0) * 1.5,
              "on the simulated hard disk, a window of 4 KiB is much slower than 1 MiB")
        found = Searcher(sim, ResultsFile(None), "M1", sim_limits, sim_defaults,
                         lambda name, value: float(value)).run(
            ("mft.window_bytes",), None)
        best = min(sim.measure({"mft.window_bytes": 1 << shift}, 0) for shift in range(12, 23))
        got = sim.measure(dict(found) or {"mft.window_bytes": 4 << 20}, 0)
        check(got <= best * (1 + GAIN_THRESHOLD),
              f"the search on the simulated disk is within {GAIN_THRESHOLD:.0%} of the best ({found})")
        replay_check = run_json([str(bench), "knobs"])
        check(any(k["name"] == "reader.max_reads" for k in replay_check["knobs"]),
              "NtfsBench lists the knobs")

    print("self-test:", "FAILED" if failures else "passed")
    return 1 if failures else 0


# ------------------------------------------------------------------ main


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true",
                        help="run the searcher on a simulated disk with a known optimum")
    parser.add_argument("--sim-exe", help="NtfsBench, to also run the self-test on its simulated disk")
    sub = parser.add_subparsers(dest="command")

    def common(p: argparse.ArgumentParser) -> None:
        p.add_argument("--bin-dir", required=True,
                       help="folder of NtfsMftList, NtfsDir, NtfsDir2 and NtfsBench, or the build tree")
        p.add_argument("--config", default="Release", help="build configuration, in a build tree")
        p.add_argument("--out-dir", required=True, help="results, CSV and report")
        p.add_argument("--timeout", type=float, default=900, help="seconds a run may take")
        p.add_argument("--no-cold", action="store_true", help="do not purge the file cache")
        p.add_argument("--no-sample", action="store_true", help="do not sample the disk")
        p.add_argument("--scratch", action="append", default=[], metavar="VOLUME=FOLDER",
                       help="where the listings of a volume go (never the scanned drive)")

    a = sub.add_parser("audit", help="T_real, T_io, T_cpu and R")
    common(a)
    a.add_argument("--volume", action="append", required=True, help="c:, e:, a device or an image")
    a.add_argument("--tools", default="mftlist,dir", type=lambda s: s.split(","))
    a.add_argument("--modes", default="none,recover,deleted,both", type=lambda s: s.split(","))
    a.add_argument("--caches", default="fullcache,nocache", type=lambda s: s.split(","))
    a.add_argument("--dir-scenarios", default="subtree,bigdir,deep", type=lambda s: s.split(","),
                   help="subtree, bigdir, deep, and full (hours on a big volume)")
    a.add_argument("--subtree", default="Windows\\System32")
    a.add_argument("--bigdir", default="Windows\\WinSxS")
    a.add_argument("--deep", default="Windows\\System32\\drivers\\etc")
    a.add_argument("--rounds", type=int, default=3)
    a.add_argument("--threads", default="1,2,4,8", type=lambda s: [int(x) for x in s.split(",") if x])
    a.add_argument("--previous", help="results.json of an earlier audit, to compare with")
    a.add_argument("--no-disk-sheet", action="store_true")
    a.add_argument("--restart", action="store_true", help="ignore the results already there")
    a.set_defaults(handler=audit)

    s = sub.add_parser("search", help="the constant search")
    common(s)
    s.add_argument("--volume", required=True)
    s.add_argument("--patterns", default="M1,M2", type=lambda x: x.split(","))
    s.add_argument("--subtree", default="Windows\\System32",
                   help="folder the M3 search lists recursively")
    s.add_argument("--profile-out", required=True)
    s.add_argument("--label")
    s.add_argument("--validate", action="store_true", help="run the full tool in its four modes with the profile")
    s.set_defaults(handler=search)

    d = sub.add_parser("disk", help="the sheet of the disk")
    d.add_argument("--bin-dir", required=True)
    d.add_argument("--config", default="Release")
    d.add_argument("--volume", required=True)
    d.add_argument("--out", required=True)
    d.add_argument("--timeout", type=float, default=900)
    d.set_defaults(handler=disk_command)

    r = sub.add_parser("report", help="render the report of an audit")
    r.add_argument("--results", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--previous")
    r.set_defaults(handler=report)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test(args)
    if not getattr(args, "handler", None):
        parser.print_help()
        return 2
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
