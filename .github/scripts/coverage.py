#!/usr/bin/env python3
"""Local code coverage for ntfs-browser (clang source-based, Windows and Linux).

usage:
  coverage.py run   [--skip-build] [--skip-tests] [--require-data]
                    [--llvm-dir DIR] [--cmake-arg ARG]...
  coverage.py merge

run    builds NtfsBrowserTests and NtfsFuzzerAfl instrumented, runs the unit
       tests twice (without, then only with the [regression] tag), and writes
       lcov, HTML and a gaps report per phase: unit, regr, all.
merge  unions the gaps of every platform found under build/coverage/ (a line
       compiled out on one OS is judged by the other OS only).

Run it once per OS:
  Windows (PowerShell):  python .github\\scripts\\coverage.py run
  WSL:                   python3 .github/scripts/coverage.py run

Why both binaries are instrumented: each regression testcase spawns
NtfsFuzzerAfl as a child process, so the library code those tests reach runs
in the child. LLVM_PROFILE_FILE carries %p, one profile per process.

Only NtfsBrowserTests and NtfsFuzzerAfl are built: the MFC samples are never
compiled.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

IS_WIN = os.name == "nt"
REPO = Path(__file__).resolve().parents[2]
PLATFORM = "windows" if IS_WIN else "linux"
COVERAGE_ROOT = REPO / "build" / "coverage"
OUT = COVERAGE_ROOT / PLATFORM

# Catch2 test specs. The corpus tests are in the first phase on purpose.
PHASES = {"unit": "~[regression]", "regr": "[regression]"}

# 3rdparty, tests, generated files and system headers are not reported.
IGNORE_REGEX = (
    r"[/\\](3rdparty|_deps|NTFSLibTests|build|obj)[/\\]"
    r"|[Pp]rogram [Ff]iles|^/usr/|^/opt/"
)

COV_FLAGS = "-fprofile-instr-generate -fcoverage-mapping"


def log(message):
    print(f"[coverage] {message}", flush=True)


def run(cmd, env=None, cwd=None, log_file=None, check=True):
    log("$ " + " ".join(str(c) for c in cmd))
    if log_file:
        with open(log_file, "w", encoding="utf-8", errors="replace") as out:
            proc = subprocess.run(
                [str(c) for c in cmd], env=env, cwd=cwd, stdout=out,
                stderr=subprocess.STDOUT)
    else:
        proc = subprocess.run([str(c) for c in cmd], env=env, cwd=cwd)
    if check and proc.returncode != 0:
        sys.exit(f"command failed with exit code {proc.returncode}")
    return proc.returncode


def work_dir():
    # A build tree on /mnt (9p) is very slow: keep it on the WSL disk.
    if not IS_WIN and str(REPO).startswith("/mnt/"):
        return Path.home() / ".cache" / "ntfs-browser-coverage"
    return OUT / "work"


def find_tools(llvm_dir):
    if IS_WIN:
        base = Path(llvm_dir or os.environ.get("LLVM_DIR", r"E:\Program\LLVM-23\bin"))
        tools = {
            "cc": base / "clang-cl.exe",
            "cxx": base / "clang-cl.exe",
            "linker": base / "lld-link.exe",
            "cov": base / "llvm-cov.exe",
            "prof": base / "llvm-profdata.exe",
        }
    else:
        def which(*names):
            for name in names:
                path = shutil.which(name)
                if path:
                    return Path(path)
            sys.exit(f"none of {names} found in PATH")

        tools = {
            "cc": which("clang-23", "clang"),
            "cxx": which("clang++-23", "clang++"),
            "cov": which("llvm-cov-23", "llvm-cov"),
            "prof": which("llvm-profdata-23", "llvm-profdata"),
        }
    for key, path in tools.items():
        if not Path(path).exists():
            sys.exit(f"{key}: {path} not found (use --llvm-dir)")
    return tools


def msvc_environment():
    """Environment of a x64 Developer Prompt (INCLUDE, LIB, PATH, ...)."""
    vswhere = (Path(os.environ["ProgramFiles(x86)"])
               / "Microsoft Visual Studio" / "Installer" / "vswhere.exe")
    install = subprocess.check_output(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"], text=True).strip()
    bat = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    dump = subprocess.check_output(f'"{bat}" >nul && set', shell=True, text=True)
    return dict(line.split("=", 1) for line in dump.splitlines() if "=" in line)


def build_environment(tools):
    env = dict(msvc_environment() if IS_WIN else os.environ)
    # CFLAGS/CXXFLAGS/LDFLAGS seed CMake's *_FLAGS_INIT: the platform's own
    # defaults (/EHsc on MSVC-like) stay, unlike with -DCMAKE_CXX_FLAGS.
    env["CFLAGS"] = COV_FLAGS
    env["CXXFLAGS"] = COV_FLAGS
    link = COV_FLAGS
    if IS_WIN:
        # CMake links with lld-link directly: the profile runtime is not
        # added by the clang driver, so name it.
        resource = subprocess.check_output(
            [str(tools["cc"]), "--print-resource-dir"], text=True).strip()
        runtime = Path(resource) / "lib" / "windows" / "clang_rt.profile-x86_64.lib"
        if not runtime.exists():
            sys.exit(f"{runtime} not found")
        link = str(runtime)
    env["LDFLAGS"] = link
    return env


def configure_and_build(tools, work, env, extra_args, skip_build):
    build = work / "build"
    if not skip_build:
        args = [
            "cmake", "-S", REPO, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug", "-DBUILD_SHARED_LIBS=OFF",
            f"-DCMAKE_C_COMPILER={tools['cc']}",
            f"-DCMAKE_CXX_COMPILER={tools['cxx']}",
        ]
        if IS_WIN:
            # Crypto++ refuses clang-cl (config_os.h): BCrypt only here.
            # The Crypto++ backend is covered on Linux.
            args += [f"-DCMAKE_LINKER={tools['linker']}",
                     "-DNTFS_BROWSER_ENABLE_EFS_CRYPTOPP=OFF"]
        run(args + extra_args, env=env)
        run(["cmake", "--build", build, "--target", "NtfsBrowserTests",
             "NtfsFuzzerAfl", "--parallel", str(os.cpu_count() or 4)], env=env)
    return build


def find_binary(build, name):
    suffix = ".exe" if IS_WIN else ""
    found = sorted(build.rglob(name + suffix))
    found = [p for p in found if p.is_file()]
    if not found:
        sys.exit(f"{name} not found under {build}: build first")
    return found[0]


def run_tests(tests_exe, work, env, require_data):
    prof = work / "prof"
    if prof.exists():
        shutil.rmtree(prof)
    for phase, spec in PHASES.items():
        directory = prof / phase
        directory.mkdir(parents=True)
        phase_env = dict(env)
        # %m turns on merging: a recycled PID (frequent on Windows) adds to
        # the old profile instead of overwriting it.
        phase_env["LLVM_PROFILE_FILE"] = str(directory / "%p-%m.profraw")
        if require_data:
            phase_env["NTFS_BROWSER_REQUIRE_TEST_DATA"] = "1"
        OUT.mkdir(parents=True, exist_ok=True)
        code = run([tests_exe, spec], env=phase_env,
                   log_file=OUT / f"tests-{phase}.log", check=False)
        count = len(list(directory.glob("*.profraw")))
        log(f"phase {phase}: exit code {code}, {count} profiles "
            f"(log: {OUT / f'tests-{phase}.log'})")


def relative(path):
    rel = os.path.relpath(path, REPO)
    if rel.startswith(".."):
        return None
    return rel.replace("\\", "/")


def parse_lcov(text):
    """Returns {file: {"hit": set, "miss": set, "branch_miss": set}}."""
    files = {}
    current = None
    counts = {}
    branch_miss = set()
    for line in text.splitlines():
        if line.startswith("SF:"):
            current = relative(line[3:])
            counts = {}
            branch_miss = set()
        elif current is None:
            continue
        elif line.startswith("DA:"):
            number, count = line[3:].split(",")[:2]
            counts[int(number)] = max(counts.get(int(number), 0), int(count))
        elif line.startswith("BRDA:"):
            number, _, _, taken = line[5:].split(",")
            if taken in ("-", "0"):
                branch_miss.add(int(number))
        elif line == "end_of_record":
            files[current] = {
                "hit": {n for n, c in counts.items() if c > 0},
                "miss": {n for n, c in counts.items() if c == 0},
                "branch_miss": branch_miss,
            }
            current = None
    return files


def ranges(numbers):
    out = []
    for n in sorted(numbers):
        if out and n == out[-1][1] + 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return ", ".join(str(a) if a == b else f"{a}-{b}" for a, b in out)


def write_gaps(files, directory, phase):
    data = {name: {k: sorted(v) for k, v in entry.items()}
            for name, entry in files.items()}
    (directory / f"gaps-{phase}.json").write_text(
        json.dumps(data, indent=0), encoding="utf-8")
    write_gaps_text(files, directory / f"gaps-{phase}.txt")


def write_gaps_text(files, path):
    rows = []
    for name, entry in files.items():
        total = len(entry["hit"]) + len(entry["miss"])
        if total and (entry["miss"] or entry["branch_miss"]):
            rows.append((len(entry["miss"]), name, entry, total))
    rows.sort(key=lambda row: (-row[0], row[1]))
    hit = sum(len(e["hit"]) for e in files.values())
    total_lines = hit + sum(len(e["miss"]) for e in files.values())
    pct = 100.0 * hit / total_lines if total_lines else 100.0
    with open(path, "w", encoding="utf-8") as out:
        out.write(f"lines: {hit}/{total_lines} ({pct:.1f}%)\n\n")
        for missed, name, entry, total in rows:
            out.write(f"{name}  {total - missed}/{total} lines\n")
            if entry["miss"]:
                out.write(f"  miss:        {ranges(entry['miss'])}\n")
            if entry["branch_miss"]:
                out.write(f"  branch_miss: {ranges(entry['branch_miss'])}\n")


def report(tools, work, tests_exe, fuzzer_exe):
    prof = work / "prof"
    OUT.mkdir(parents=True, exist_ok=True)
    groups = {phase: [prof / phase] for phase in PHASES}
    groups["all"] = [prof / phase for phase in PHASES]
    for phase, dirs in groups.items():
        raws = [str(p) for d in dirs for p in sorted(d.glob("*.profraw"))]
        if not raws:
            log(f"phase {phase}: no profile, skipped")
            continue
        profdata = work / f"{phase}.profdata"
        run([tools["prof"], "merge", "-sparse", "-o", profdata] + raws)
        common = [f"-instr-profile={profdata}", tests_exe,
                  f"-object={fuzzer_exe}",
                  f"-ignore-filename-regex={IGNORE_REGEX}"]
        lcov = subprocess.check_output(
            [str(tools["cov"]), "export", "-format=lcov"] + [str(c) for c in common],
            text=True, encoding="utf-8", errors="replace")
        (OUT / f"{phase}.lcov").write_text(lcov, encoding="utf-8")
        write_gaps(parse_lcov(lcov), OUT, phase)
        run([tools["cov"], "show", "-format=html", f"-output-dir={OUT / ('html-' + phase)}",
             "-show-branches=count", "-show-instantiation-summary"] + common)
        log(f"phase {phase}: {OUT / f'gaps-{phase}.txt'}")


def merge_platforms():
    for phase in list(PHASES) + ["all"]:
        merged = {}
        sources = sorted(COVERAGE_ROOT.glob(f"*/gaps-{phase}.json"))
        sources = [s for s in sources if s.parent.name != "merged"]
        for source in sources:
            data = json.loads(source.read_text(encoding="utf-8"))
            for name, entry in data.items():
                slot = merged.setdefault(name, {"hit": set(), "miss": set(),
                                                "branch_miss": None})
                slot["hit"] |= set(entry["hit"])
                slot["miss"] |= set(entry["miss"])
                # A branch is missed overall only if every OS that has it misses it.
                if entry["branch_miss"] or slot["branch_miss"] is None:
                    mine = set(entry["branch_miss"])
                    slot["branch_miss"] = (mine if slot["branch_miss"] is None
                                           else slot["branch_miss"] & mine)
        for entry in merged.values():
            entry["miss"] -= entry["hit"]
            entry["branch_miss"] = entry["branch_miss"] or set()
        if sources:
            directory = COVERAGE_ROOT / "merged"
            directory.mkdir(parents=True, exist_ok=True)
            write_gaps(merged, directory, phase)
            log(f"{phase}: {len(sources)} platform(s) -> "
                f"{directory / f'gaps-{phase}.txt'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawTextHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    run_parser = sub.add_parser("run")
    run_parser.add_argument("--skip-build", action="store_true")
    run_parser.add_argument("--skip-tests", action="store_true",
                            help="reuse the profiles of the last run")
    run_parser.add_argument("--require-data", action="store_true",
                            help="fail instead of skip when a corpus image is absent")
    run_parser.add_argument("--llvm-dir")
    run_parser.add_argument("--cmake-arg", action="append", default=[],
                            help="extra cmake configure argument, e.g. "
                                 "-DNTFS_BROWSER_TEST_DFTT_DIR=...")
    sub.add_parser("merge")
    args = parser.parse_args()

    if args.command == "merge":
        merge_platforms()
        return

    tools = find_tools(args.llvm_dir)
    work = work_dir()
    env = build_environment(tools)
    build = configure_and_build(tools, work, env, args.cmake_arg, args.skip_build)
    tests_exe = find_binary(build, "NtfsBrowserTests")
    fuzzer_exe = find_binary(build, "NtfsFuzzerAfl")
    if not args.skip_tests:
        run_tests(tests_exe, work, env, args.require_data)
    report(tools, work, tests_exe, fuzzer_exe)


if __name__ == "__main__":
    main()
