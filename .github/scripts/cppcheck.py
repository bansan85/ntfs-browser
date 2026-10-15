#!/usr/bin/env python3
"""cppcheck 2.22 run for ntfs-browser, with every analysis option switched on.

usage:
  cppcheck.py run --project FILE --platform NAME [--library LIB]...
                  [--project-configuration CONFIG] [--clang EXE]
                  [--cppcheck EXE] [--jobs N] --out DIR

FILE is a compile_commands.json (Linux) or a Visual Studio solution
(Windows, with --project-configuration such as Debug|x64). Sources under
3rdparty/ are left out: skipped as translation units, and their findings
dropped where they come from a header.

Writes to DIR (the cppcheck cache lives in DIR/cache):
  cppcheck.sarif    every finding, SARIF 2.1.0
  cppcheck.txt      the same findings, one per line
  check-config.txt  --check-config: preprocessor configuration checks
  checkers.txt      --checkers-report: which checkers ran
  clang.sarif       --clang: experimental Clang parser, only with --clang
  summary.md        finding counts per check, for the job summary

The main and check-config passes gate the exit code. The clang pass does
not: it is experimental.

A pass counts as done when it wrote its report. cppcheck exits 1 for bad
arguments, and with --safety also when it hits a critical error, so the
exit code alone cannot tell a failed pass from a clean one with errors.
"""

import argparse
import json
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

# -i skips third-party translation units; --suppress drops the findings
# reported inside third-party headers that the project includes.
IGNORE = "*3rdparty*"
SUPPRESS = "*:*3rdparty*"

# MFC headers stop with #error unless StdAfx.h defined this guard first.
# Defining it here lets cppcheck read the MFC demo apps.
DEFINES = ["-D__AFXWIN_H__"]

# The VS generator writes .slnx from VS 2026 on, .sln before.
SOLUTION_SUFFIXES = (".sln", ".slnx")

# Every analysis switch of cppcheck 2.22, bar the ones that change the output
# shape rather than the findings: --dump, -E, --errorlist. --rule-file is
# left out: the project has no rules file.
ANALYSIS = [
    "--enable=all",
    "--inconclusive",
    "--check-level=exhaustive",
    "--force",
    "--max-configs=64",
    "--max-ctu-depth=4",
    "--check-library",
    "--safety",
    "--inline-suppr",
    "--std=c++20",
    "--report-type=misra-cpp-2023",
    "--addon=misra",
    "--addon=threadsafety",
    "--addon=y2038",
    "--addon=findcasts",
    "--addon=misc",
]


def project_args(args):
    if args.project.suffix in SOLUTION_SUFFIXES:
        return [f"--project={args.project}", f"--project-configuration={args.project_configuration}"]
    return [f"--project={args.project}"]


def common_args(args):
    return [
        args.cppcheck,
        *project_args(args),
        *ANALYSIS,
        *DEFINES,
        "-i",
        IGNORE,
        f"--suppress={SUPPRESS}",
        f"--platform={args.platform}",
        *[f"--library={lib}" for lib in args.library],
        f"-j{args.jobs}",
        "-q",
    ]


def run_pass(name, cmd, report, log=None):
    """Run one pass; return True when it wrote its report.

    With log, the pass's console output goes to that file: --check-config
    writes its findings to the console, not to --output-file.
    """
    print(f"::group::cppcheck {name}", flush=True)
    if log is None:
        rc = subprocess.run(cmd).returncode
    else:
        with log.open("w", encoding="utf-8") as stream:
            rc = subprocess.run(cmd, stdout=stream, stderr=subprocess.STDOUT).returncode
    print("::endgroup::", flush=True)
    done = report.exists()
    print(f"cppcheck {name}: exit {rc}, report {'written' if done else 'missing'}", flush=True)
    return done


def sarif_findings(path):
    """Yield (check, level, file, line, message) for each SARIF result."""
    data = json.loads(path.read_text(encoding="utf-8"))
    for run in data.get("runs", []):
        for result in run.get("results", []):
            location = (result.get("locations") or [{}])[0].get("physicalLocation", {})
            uri = location.get("artifactLocation", {}).get("uri", "?")
            line = location.get("region", {}).get("startLine", 0)
            message = result.get("message", {}).get("text", "")
            yield result.get("ruleId", "?"), result.get("level", "?"), uri, line, message


def write_text_report(sarif, text):
    findings = sorted(sarif_findings(sarif), key=lambda f: (f[2], f[3], f[0]))
    lines = [f"{uri}:{line}: {level}: {message} [{check}]" for check, level, uri, line, message in findings]
    text.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_summary(sarif, clang_sarif, summary):
    lines = ["## cppcheck", ""]
    for title, path in (("main pass", sarif), ("clang parser (experimental)", clang_sarif)):
        if not path.exists():
            lines += [f"**{title}**: no report", ""]
            continue
        counts = Counter(check for check, *_ in sarif_findings(path))
        lines += [f"**{title}**: {sum(counts.values())} findings", ""]
        if counts:
            lines += ["| Check | Count |", "|---|---:|"]
            lines += [f"| `{check}` | {n} |" for check, n in counts.most_common()]
            lines.append("")
    summary.write_text("\n".join(lines), encoding="utf-8")


def run(args):
    out = args.out
    cache = out / "cache"
    cache.mkdir(parents=True, exist_ok=True)

    sarif = out / "cppcheck.sarif"
    gating = [
        run_pass(
            "main",
            [
                *common_args(args),
                f"--cppcheck-build-dir={cache}",
                f"--checkers-report={out / 'checkers.txt'}",
                "--output-format=sarif",
                f"--output-file={sarif}",
            ],
            sarif,
        )
    ]
    if sarif.exists():
        write_text_report(sarif, out / "cppcheck.txt")

    config = out / "check-config.txt"
    gating.append(
        run_pass(
            "check-config",
            [
                args.cppcheck,
                *project_args(args),
                "--check-config",
                *DEFINES,
                "-i",
                IGNORE,
                f"--suppress={SUPPRESS}",
                f"--platform={args.platform}",
                *[f"--library={lib}" for lib in args.library],
                f"-j{args.jobs}",
                "-q",
            ],
            config,
            log=config,
        )
    )

    clang_sarif = out / "clang.sarif"
    if args.clang:
        if not run_pass(
            "clang",
            [
                *common_args(args),
                f"--cppcheck-build-dir={cache}",
                f"--clang={args.clang}",
                "--output-format=sarif",
                f"--output-file={clang_sarif}",
            ],
            clang_sarif,
        ):
            print("::warning::cppcheck clang parser pass produced no report (experimental)", flush=True)

    write_summary(sarif, clang_sarif, out / "summary.md")
    return 0 if all(gating) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subcommands = parser.add_subparsers(dest="command", required=True)
    run_parser = subcommands.add_parser("run")
    run_parser.add_argument("--project", required=True, type=Path)
    run_parser.add_argument("--project-configuration", help="Visual Studio solutions only, e.g. Debug|x64")
    run_parser.add_argument("--platform", required=True, help="cppcheck platform, e.g. unix64 or win64")
    run_parser.add_argument("--library", action="append", default=[], help="cppcheck library, repeatable")
    run_parser.add_argument("--clang", metavar="EXE", help="also run the experimental Clang parser pass")
    run_parser.add_argument("--cppcheck", default="cppcheck", help="cppcheck executable")
    run_parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    run_parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    if args.project.suffix in SOLUTION_SUFFIXES and not args.project_configuration:
        parser.error("a Visual Studio solution needs --project-configuration")
    sys.exit(run(args))


if __name__ == "__main__":
    main()
