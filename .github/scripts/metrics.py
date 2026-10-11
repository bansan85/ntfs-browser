#!/usr/bin/env python3
"""Code metrics for ntfs-browser, computed with lizard (C and C++).

usage:
  metrics.py [--paths PATH...] [--out DIR] [--top N] [--exclude PATTERN]...

Writes to DIR (default build/metrics):
  functions.csv  one row per function, every per-function metric
  files.csv      one row per file: function count and worst values
  duplicates.csv one row per duplicated code snippet, grouped by block
  summary.json   totals, averages, warning counts, duplication
  summary.md     the summary, then the top N functions per metric

lizard is pinned: python -m pip install lizard==1.24.1
"""

import argparse
import csv
import json
import sys
from collections import defaultdict
from importlib.metadata import version
from pathlib import Path

try:
    import lizard
except ImportError:
    sys.exit("lizard not found: python -m pip install lizard==1.24.1")

REPO = Path(__file__).resolve().parents[2]
DEFAULT_PATHS = ["include", "src", "NTFSLibTests"]
DEFAULT_OUT = REPO / "build" / "metrics"
DEFAULT_EXCLUDES = ["*/3rdparty/*", "*/build/*"]

# Extensions of the base analysis. None of them rewrites the CCN.
BASE_EXTENSIONS = [
    "cognitive", "halstead", "nd", "ns", "io", "exitcount", "gotocount",
    "statementcount", "dependencycount", "duplicated_param_list",
    "boolcount", "duplicate", "complextags",
]

# Warning limits: lizard's own CLI defaults (-C, -L, -a).
WARN_CCN = 15
WARN_LENGTH = 1000
WARN_PARAMS = 100

# Minimum token run that counts as duplicated code: lizard's default.
DUPLICATE_MIN_TOKENS = 70

# Base metrics: (CSV column, lizard attribute, meaning).
FUNCTION_METRICS = [
    ("nloc", "nloc", "lines of code, no comments or blank lines"),
    ("ccn", "cyclomatic_complexity", "cyclomatic complexity"),
    ("tokens", "token_count", "tokens"),
    ("params", "parameter_count", "parameters"),
    ("length", "length", "lines from first to last line"),
    ("cognitive", "cognitive_complexity", "cognitive complexity"),
    ("nesting_depth", "max_nesting_depth", "maximum nesting depth"),
    ("nested_structures", "max_nested_structures", "maximum nested control structures"),
    ("halstead_volume", "halstead_volume", "Halstead volume"),
    ("halstead_difficulty", "halstead_difficulty", "Halstead difficulty"),
    ("halstead_effort", "halstead_effort", "Halstead effort"),
    ("fan_in", "fan_in", "fan-in"),
    ("fan_out", "fan_out", "fan-out"),
    ("general_fan_out", "general_fan_out", "general fan-out"),
    ("exits", "exit_count", "exit points (return, throw...)"),
    ("gotos", "goto_count", "goto statements"),
    ("statements", "statement_count", "statements"),
    ("dependencies", "dependency_count", "dependencies"),
    ("dup_params", "parameter_list_duplicates", "parameter lists duplicated in the file"),
    ("dup_params_files", "parameter_list_duplicated_in_files",
     "parameter lists duplicated across files"),
]

# Variant metrics. Each variant runs its own analysis, because a variant that
# rewrites the CCN would change the base value. Entries: (extensions, metrics).
VARIANTS = [
    (["nd", "mccabe"], [
        ("ccn_mccabe", "cyclomatic_complexity", "cyclomatic complexity, McCabe definition"),
        ("nd_mccabe", "max_nesting_depth", "nesting depth, McCabe definition"),
    ]),
    (["modified"], [
        ("ccn_modified", "cyclomatic_complexity", "cyclomatic complexity, switch counted once"),
    ]),
    (["nonstrict"], [
        ("ccn_no_logic", "cyclomatic_complexity",
         "cyclomatic complexity, && || and or not ! not counted"),
    ]),
    (["cpre"], [
        ("ccn_cpre", "cyclomatic_complexity", "cyclomatic complexity, #else branches ignored"),
    ]),
    (["ignoreassert"], [
        ("ccn_ignoreassert", "cyclomatic_complexity", "cyclomatic complexity, asserts ignored"),
    ]),
]

# Columns that hold a CCN: each one gets a warning count in summary.md.
CCN_COLUMNS = ["ccn", "ccn_mccabe", "ccn_modified", "ccn_no_logic",
               "ccn_cpre", "ccn_ignoreassert"]

# Metrics that get a top-N table in summary.md.
TOP_METRICS = ["ccn", "ccn_mccabe", "cognitive", "nesting_depth", "nd_mccabe",
               "nested_structures", "length", "params", "nloc", "halstead_effort",
               "fan_out", "exits", "duplicate_blocks"]

# Columns that hold a number, in CSV order. The per-function table adds
# duplicate_blocks and complex_tags after them.
NUMERIC_METRICS = [(c, m) for c, _, m in FUNCTION_METRICS] + [
    (c, m) for _, metrics in VARIANTS for c, _, m in metrics
] + [("duplicate_blocks", "duplicated blocks the function is part of")]
MEANINGS = dict(NUMERIC_METRICS)
NUMERIC_COLUMNS = [column for column, _ in NUMERIC_METRICS]
FUNCTION_COLUMNS = ["file", "function", "start_line", "end_line",
                    *NUMERIC_COLUMNS, "complex_tags"]
FILE_COLUMNS = ["file", "functions", "file_nloc", "sum_ccn", "max_ccn", "max_cognitive",
                "max_nesting_depth", "max_length", "max_params"]


def rel(name):
    try:
        return Path(name).resolve().relative_to(REPO).as_posix()
    except ValueError:
        return Path(name).as_posix()


def number(value):
    return round(value, 2) if isinstance(value, float) else value


def mean(values):
    return sum(values) / len(values) if values else 0.0


def analyze(extensions, paths, excludes):
    exts = lizard.get_extensions(extensions)
    infos = [info for info in lizard.analyze(
        paths, exclude_pattern=excludes, exts=exts, lans=["cpp"]) if info]
    return exts, infos


def function_row(fun):
    row = {"file": rel(fun.filename), "function": fun.long_name,
           "start_line": fun.start_line, "end_line": fun.end_line}
    for column, attribute, _ in FUNCTION_METRICS:
        row[column] = number(getattr(fun, attribute))
    row["complex_tags"] = " ".join(f"{keyword}:{line}"
                                   for keyword, line in getattr(fun, "complex_tags", []))
    return row


def file_row(info):
    functions = info.function_list
    return {
        "file": rel(info.filename),
        "functions": len(functions),
        "file_nloc": info.nloc,
        "sum_ccn": sum(f.cyclomatic_complexity for f in functions),
        "max_ccn": max((f.cyclomatic_complexity for f in functions), default=0),
        "max_cognitive": max((f.cognitive_complexity for f in functions), default=0),
        "max_nesting_depth": max((f.max_nesting_depth for f in functions), default=0),
        "max_length": max((f.length for f in functions), default=0),
        "max_params": max((f.parameter_count for f in functions), default=0),
    }


def add_variants(rows, paths, excludes):
    """Adds each variant's columns to the rows. Returns unmatched counts per variant."""
    by_key = {(row["file"], row["start_line"]): row for row in rows}
    unmatched = {}
    for extensions, metrics in VARIANTS:
        _, infos = analyze(extensions, paths, excludes)
        name = "+".join(extensions)
        unmatched[name] = 0
        for info in infos:
            for fun in info.function_list:
                row = by_key.get((rel(fun.filename), fun.start_line))
                if row is None:
                    unmatched[name] += 1
                    continue
                for column, attribute, _ in metrics:
                    row[column] = number(getattr(fun, attribute))
    return unmatched


def count_duplicate_blocks(rows, blocks):
    """Sets duplicate_blocks on each row: the blocks that overlap the function."""
    by_file = defaultdict(list)
    for row in rows:
        row["duplicate_blocks"] = 0
        by_file[row["file"]].append(row)
    for block in blocks:
        touched = {}
        for snippet in block:
            for row in by_file.get(rel(snippet.file_name), []):
                if snippet.start_line <= row["end_line"] and row["start_line"] <= snippet.end_line:
                    touched[id(row)] = row
        for row in touched.values():
            row["duplicate_blocks"] += 1


def write_csv(path, rows, columns):
    with open(path, "w", newline="", encoding="utf-8") as out:
        writer = csv.DictWriter(out, fieldnames=columns, restval="")
        writer.writeheader()
        writer.writerows(rows)


def write_duplicates(path, blocks):
    rows = [{"block": index, "file": rel(snippet.file_name),
             "start_line": snippet.start_line, "end_line": snippet.end_line}
            for index, block in enumerate(blocks, 1) for snippet in block]
    write_csv(path, rows, ["block", "file", "start_line", "end_line"])


def present(rows, column):
    return [row[column] for row in rows if row.get(column) is not None]


def summarize(rows, infos, blocks, duplicates, bools, unmatched):
    return {
        "lizard": version("lizard"),
        "files": len(infos),
        "functions": len(rows),
        "nloc_in_functions": sum(row["nloc"] for row in rows),
        "nloc_in_files": sum(info.nloc for info in infos),
        "average": {c: number(mean(present(rows, c))) for c in NUMERIC_COLUMNS},
        "maximum": {c: number(max(present(rows, c), default=0)) for c in NUMERIC_COLUMNS},
        "warnings": {
            **{f"{c} > {WARN_CCN}": sum(v > WARN_CCN for v in present(rows, c))
               for c in CCN_COLUMNS},
            f"length > {WARN_LENGTH}": sum(v > WARN_LENGTH for v in present(rows, "length")),
            f"params > {WARN_PARAMS}": sum(v > WARN_PARAMS for v in present(rows, "params")),
        },
        "unmatched_variant_functions": unmatched,
        "duplication": {
            "blocks": len(blocks),
            "duplicate_rate_percent": number(duplicates.duplicate_rate() * 100),
            "unique_rate_percent": number(duplicates.unique_rate() * 100),
        },
        "boolean_operators": {
            "bool_tokens": bools.total_bool,
            "tokens": bools.total_token,
            "rate_percent": number(100.0 * bools.total_bool / max(bools.total_token, 1)),
        },
    }


def write_markdown(path, summary, rows, top):
    lines = [
        "# ntfs-browser code metrics",
        "",
        f"lizard {summary['lizard']}: {summary['functions']} functions in "
        f"{summary['files']} files, {summary['nloc_in_functions']} lines in functions.",
        "",
        "## Warnings",
        "",
        "| Limit | Functions |",
        "|---|---|",
    ]
    lines += [f"| {limit} | {count} |" for limit, count in summary["warnings"].items()]
    dup = summary["duplication"]
    bools = summary["boolean_operators"]
    lines += [
        "",
        f"Duplicated blocks: {dup['blocks']} ({dup['duplicate_rate_percent']}% of tokens).",
        f"Boolean operators: {bools['rate_percent']}% of tokens.",
    ]
    for variant, count in summary["unmatched_variant_functions"].items():
        if count:
            lines.append(f"Variant {variant}: {count} functions not matched to the base run.")
    lines += [
        "",
        "## Averages and maxima per function",
        "",
        "| Metric | Average | Maximum |",
        "|---|---|---|",
    ]
    lines += [f"| {MEANINGS[column]} | {summary['average'][column]} | "
              f"{summary['maximum'][column]} |" for column in NUMERIC_COLUMNS]
    for column in TOP_METRICS:
        ranked = sorted(rows, key=lambda r: r.get(column) if r.get(column) is not None else -1,
                        reverse=True)[:top]
        lines += ["", f"## Top {top}: {MEANINGS[column]}", "",
                  f"| {MEANINGS[column]} | Function | Location |", "|---|---|---|"]
        lines += [f"| {row[column]} | `{row['function']}` | "
                  f"{row['file']}:{row['start_line']} |" for row in ranked]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--paths", nargs="+", default=DEFAULT_PATHS,
                        help="relative to the repository root (default: include src NTFSLibTests)")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--top", type=int, default=10,
                        help="rows per top table in summary.md")
    parser.add_argument("--exclude", action="append",
                        help="lizard glob, repeatable (default: 3rdparty and build)")
    args = parser.parse_args()

    paths = [str(REPO / p) for p in args.paths]
    excludes = args.exclude or DEFAULT_EXCLUDES
    exts, infos = analyze(BASE_EXTENSIONS, paths, excludes)
    rows = [function_row(fun) for info in infos for fun in info.function_list]
    unmatched = add_variants(rows, paths, excludes)

    # The extensions with run-wide results: read them after the analysis.
    duplicates = next(e for e in exts if hasattr(e, "get_duplicates"))
    blocks = list(duplicates.get_duplicates(DUPLICATE_MIN_TOKENS))
    bools = next(e for e in exts if hasattr(e, "total_bool"))
    count_duplicate_blocks(rows, blocks)

    args.out.mkdir(parents=True, exist_ok=True)
    write_csv(args.out / "functions.csv", rows, FUNCTION_COLUMNS)
    write_csv(args.out / "files.csv", [file_row(info) for info in infos], FILE_COLUMNS)
    write_duplicates(args.out / "duplicates.csv", blocks)
    summary = summarize(rows, infos, blocks, duplicates, bools, unmatched)
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8")
    write_markdown(args.out / "summary.md", summary, rows, args.top)
    print(f"{summary['functions']} functions in {summary['files']} files, "
          f"{len(blocks)} duplicated blocks -> {args.out}")


if __name__ == "__main__":
    main()
