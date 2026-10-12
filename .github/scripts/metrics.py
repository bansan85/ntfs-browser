#!/usr/bin/env python3
"""Code metrics for ntfs-browser, computed with lizard (C and C++).

usage:
  metrics.py [--paths PATH...] [--out DIR] [--top N] [--exclude PATTERN]...
             [--coverage GAPS_JSON] [--no-churn]

Writes to DIR (default build/metrics):
  functions.csv  one row per function, every per-function metric
  files.csv      one row per file: function count and worst values
  duplicates.csv one row per duplicated code snippet, grouped by block
  summary.json   totals, averages, limit counts, duplication, coverage
  includes.csv   one row per file: include and dependency counts, cycles
  classes.csv    one row per class: WMC, LCOM4, CBO (a text scan, see class_rows)
  summary.md     the summary, the limits, the breaches beyond them, then the top N
                 functions and classes per metric

--coverage reads a gaps-all.json written by coverage.py (default: the
merged CI result, build/coverage/merged/gaps-all.json). Without that file
the coverage columns stay empty.

Limits have two tiers. A value above the alert asks for a look; a value above
the limit asks for a refactoring. The exit code does not change.

Churn comes from git history: one git log -L per function, one per file
(about 3 minutes for the whole tree). It follows the lines of HEAD, so
uncommitted edits shift the ranges. --no-churn skips it.

lizard is pinned: python -m pip install lizard==1.24.1
"""

import argparse
import bisect
import csv
import json
import posixpath
import re
import subprocess
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
# Merged CI coverage, all phases: the unit tests and the fuzz regression replays.
DEFAULT_COVERAGE = REPO / "build" / "coverage" / "merged" / "gaps-all.json"

# Commit subjects that count as fixes: the Conventional Commits prefix, or the
# older "Fix ..." style.
FIX_SUBJECT = re.compile(r"^fix\b", re.IGNORECASE)

# git log format: sha, author date, subject, separated by tabs.
GIT_LOG_FORMAT = "--format=%H%x09%ad%x09%s"

# An #include directive, with its name in group 1: <...> or "...".
INCLUDE_LINE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

# Files whose #include lines form the coupling graph.
SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".c")

# Extensions of the base analysis. None of them rewrites the CCN.
BASE_EXTENSIONS = [
    "cognitive", "halstead", "nd", "ns", "io", "exitcount", "gotocount",
    "statementcount", "dependencycount", "duplicated_param_list",
    "boolcount", "duplicate", "complextags",
]

# Two tiers per indicator, each (alert, limit). A value strictly above the alert asks
# for a look; strictly above the limit asks for a refactoring. Values calibrated on the
# library's own distribution, p90 to p99, 2026-10. They never fail the run.
# Per-function limits. The CCN variants share the CCN pair.
FUNCTION_LIMITS = {
    "ccn": (10, 15),
    "ccn_mccabe": (10, 15),
    "ccn_modified": (10, 15),
    "ccn_no_logic": (10, 15),
    "ccn_cpre": (10, 15),
    "ccn_ignoreassert": (10, 15),
    "cognitive": (15, 20),
    "nloc": (40, 60),
    "nesting_depth": (3, 4),
    "params": (4, 5),
    "fan_out": (10, 15),
    "exits": (6, 8),
}
# Per-class limits. LCOM4 above 1 means the methods split into groups that share
# no member and no call.
CLASS_LIMITS = {"wmc": (50, 100), "cbo": (10, 20), "lcom4": (1, 2)}
# Per-file limit on lines of code.
FILE_LIMITS = {"file_nloc": (400, 800)}
# Per-file limit on the files it includes directly.
INCLUDE_LIMITS = {"includes_out": (15, 25)}
# Table labels for the class, file and include columns, which MEANINGS does not hold.
LIMIT_LABELS = {
    "wmc": "weighted methods per class, WMC",
    "cbo": "coupling between objects, CBO",
    "lcom4": "lack of cohesion, LCOM4 components",
    "file_nloc": "lines of code in the file",
    "includes_out": "files the file includes directly",
}
# Include cycles allowed. Any cycle is beyond this limit.
CYCLE_LIMIT = 0
# A heavy function is above HEAVY_CCN in CCN or above HEAVY_NLOC in lines.
# Both equal their alerts, so a heavy function is one already asked to be looked at.
HEAVY_CCN = 10
HEAVY_NLOC = 40
# Coverage prerequisite for a heavy function. Agreed values, not derived from the data.
MIN_COVERAGE_PCT = 90
MAX_BRANCH_GAP_LINES = 2

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

# Columns that hold a CCN: each one is checked against the CCN limits.
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
] + [
    ("duplicate_blocks", "duplicated blocks the function is part of"),
    ("coverage_pct", "line coverage, percent"),
    ("branch_gap_lines", "lines with a branch never taken"),
    ("churn_commits", "commits that changed the function"),
    ("churn_fixes", "fix commits that changed the function"),
]
MEANINGS = dict(NUMERIC_METRICS)
NUMERIC_COLUMNS = [column for column, _ in NUMERIC_METRICS]
FUNCTION_COLUMNS = ["file", "function", "start_line", "end_line",
                    *NUMERIC_COLUMNS, "uncovered_lines", "churn_last_change",
                    "complex_tags"]
INCLUDE_COLUMNS = ["file", "includes_out", "includes_in", "depends_on", "dependents",
                   "instability", "cycle"]
FILE_COLUMNS = ["file", "functions", "file_nloc", "sum_ccn", "max_ccn", "max_cognitive",
                "max_nesting_depth", "max_length", "max_params",
                "file_commits", "file_fixes", "file_last_change"]


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


def load_coverage(path):
    """Returns {repo-relative file: {"hit", "miss", "branch_miss"}} of line sets."""
    data = json.loads(path.read_text(encoding="utf-8"))
    return {name: {key: set(lines) for key, lines in entry.items()}
            for name, entry in data.items()}


def line_ranges(numbers):
    out = []
    for n in sorted(numbers):
        if out and n == out[-1][1] + 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return ", ".join(str(a) if a == b else f"{a}-{b}" for a, b in out)


def add_coverage(rows, coverage):
    """Sets the coverage columns of each row from the lines inside its function."""
    for row in rows:
        entry = coverage.get(row["file"])
        if entry is None:
            # coverage.py has no data for this file: no line was built or run.
            row.update(coverage_pct=None, branch_gap_lines=None, uncovered_lines="")
            continue
        low, high = row["start_line"], row["end_line"]
        hit = {n for n in entry["hit"] if low <= n <= high}
        miss = {n for n in entry["miss"] if low <= n <= high}
        executable = hit | miss
        row["coverage_pct"] = (number(100.0 * len(hit) / len(executable))
                               if executable else None)
        row["branch_gap_lines"] = sum(low <= n <= high for n in entry["branch_miss"])
        row["uncovered_lines"] = line_ranges(miss)


def git_log(*args):
    """Returns (sha, date, subject) per commit, newest first. None if git fails."""
    result = subprocess.run(
        ["git", "-C", str(REPO), "log", GIT_LOG_FORMAT, "--date=short", *args],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        return None
    commits = []
    for line in result.stdout.splitlines():
        parts = line.split("\t", 2)
        if len(parts) == 3 and len(parts[0]) == 40:
            commits.append(tuple(parts))
    return commits


def churn_values(commits):
    """Returns (commit count, fix count, date of the newest commit)."""
    if commits is None:
        return None, None, ""
    fixes = sum(bool(FIX_SUBJECT.match(subject)) for _, _, subject in commits)
    return len(commits), fixes, commits[0][1] if commits else ""


def add_function_churn(rows):
    """Sets the churn columns from the history of each function's lines."""
    for row in rows:
        # -L follows the function's line range back through the history.
        commits = git_log(f"-L{row['start_line']},{row['end_line']}:{row['file']}", "-s")
        (row["churn_commits"], row["churn_fixes"],
         row["churn_last_change"]) = churn_values(commits)


def add_file_churn(file_rows):
    """Sets the file churn columns, following renames."""
    for row in file_rows:
        (row["file_commits"], row["file_fixes"],
         row["file_last_change"]) = churn_values(git_log("--follow", "--", row["file"]))


def tracked_sources(paths):
    """Returns the tracked C and C++ files under the given paths, as posix paths."""
    result = subprocess.run(
        ["git", "-C", str(REPO), "ls-files", "--", *paths],
        capture_output=True, text=True, encoding="utf-8", check=True)
    return [line for line in result.stdout.splitlines() if line.endswith(SOURCE_SUFFIXES)]


def resolve_include(owner, name, known, by_suffix):
    """Returns (target file or None, whether the name was ambiguous)."""
    # A quoted include is looked up next to the including file first.
    local = posixpath.normpath(posixpath.join(posixpath.dirname(owner), name))
    if local in known:
        return local, False
    matches = by_suffix.get(name, [])
    if len(matches) == 1:
        return matches[0], False
    return None, len(matches) > 1


def include_graph(files):
    """Returns ({file: files it includes}, ambiguous include count)."""
    known = set(files)
    by_suffix = defaultdict(list)
    for path in files:
        parts = path.split("/")
        for start in range(len(parts)):
            by_suffix["/".join(parts[start:])].append(path)
    graph = {path: set() for path in files}
    ambiguous = 0
    for path in files:
        text = (REPO / path).read_text(encoding="utf-8", errors="replace")
        for name in INCLUDE_LINE.findall(text):
            target, is_ambiguous = resolve_include(path, name, known, by_suffix)
            ambiguous += is_ambiguous
            # Includes of system and third-party headers resolve to no file here.
            if target is not None and target != path:
                graph[path].add(target)
    return graph, ambiguous


def reachable(start, edges):
    """Returns the files reached from start by following edges, start excluded."""
    seen, stack = set(), list(edges[start])
    while stack:
        node = stack.pop()
        if node != start and node not in seen:
            seen.add(node)
            stack.extend(edges[node])
    return seen


def include_cycles(graph):
    """Returns the groups of two or more files that include each other, directly or not."""
    # Tarjan's algorithm: one strongly connected component per cycle group.
    index, low, on_stack, stack, groups = {}, {}, set(), [], []

    def visit(node):
        index[node] = low[node] = len(index)
        stack.append(node)
        on_stack.add(node)
        for target in graph[node]:
            if target not in index:
                visit(target)
                low[node] = min(low[node], low[target])
            elif target in on_stack:
                low[node] = min(low[node], index[target])
        if low[node] == index[node]:
            group = []
            while True:
                member = stack.pop()
                on_stack.discard(member)
                group.append(member)
                if member == node:
                    break
            if len(group) > 1:
                groups.append(sorted(group))

    for node in graph:
        if node not in index:
            visit(node)
    return groups


def coupling_rows(graph):
    """Returns one row per file with its include and dependency counts."""
    reverse = {path: set() for path in graph}
    for path, targets in graph.items():
        for target in targets:
            reverse[target].add(path)
    cycles = include_cycles(graph)
    cycle_of = {member: number for number, group in enumerate(cycles, 1) for member in group}
    rows = []
    for path in sorted(graph):
        out, back = len(graph[path]), len(reverse[path])
        rows.append({
            "file": path,
            "includes_out": out,
            "includes_in": back,
            "depends_on": len(reachable(path, graph)),
            "dependents": len(reachable(path, reverse)),
            # Martin's instability: 1 means it only depends on others.
            "instability": number(out / (out + back)) if out + back else None,
            "cycle": cycle_of.get(path, ""),
        })
    return rows, cycles


# Class scanning: text only. A class is a brace scope, its data members are the
# declarations at its top level, and its methods come from lizard's names.
COMMENT_OR_LITERAL = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL)
# A preprocessor line, blanked so that its braces and quotes do not count.
PREPROCESSOR_LINE = re.compile(r"^[ \t]*#[^\n]*", re.MULTILINE)
# A scope boundary: the characters that open, close or end a statement.
BOUNDARY = re.compile(r"[{};]")
# One innermost template argument list.
TEMPLATE_ARGS = re.compile(r"<[^<>]*>")
# A class, struct or union header at the brace. Upper-case words before the
# name are export macros; the qualified name is group 1.
CLASS_HEADER = re.compile(
    r"\b(?:class|struct|union)\s+(?:[A-Z][A-Z0-9_]*\s+)*"
    r"((?:[A-Za-z_]\w*(?:<[^<>{};()]*>)?::)*[A-Za-z_]\w*(?:<[^<>{};()]*>)?)"
    r"\s*(?:final\s*)?(?::[^;{}]*)?$")
# An access label such as "public:", removed before a declaration is read.
ACCESS_LABEL = re.compile(r"\b(?:public|private|protected)\s*:(?!:)")
# Declarations that never define a per-object data member.
NOT_DATA_MEMBER = re.compile(
    r"^(?:using|typedef|friend|static_assert|enum|class|struct|union|template|namespace)\b"
    r"|\b(?:static|constexpr|operator)\b")
# The export macro lizard prints in front of a class name, such as NTFS_BROWSER_EXPORT.
EXPORT_PREFIX = re.compile(r"[A-Z][A-Z0-9_]*")
# An identifier. The \b keeps the letters of a number such as 0x1F out.
IDENT = re.compile(r"\b[A-Za-z_]\w*")
# A name with the scopes written in front of it, such as Data::Filename.
QUALIFIED_NAME = re.compile(r"\b(?:[A-Za-z_]\w*\s*::\s*)*[A-Za-z_]\w*")
# A call: a name followed by an opening parenthesis.
CALL = re.compile(r"\s*\(")
# The object of an arrow that is this class's own: this-> or impl_->.
OWN_ARROW = re.compile(r"\b(?:this|impl_)\s*->$")
CLASS_COLUMNS = ["file", "class", "start_line", "end_line", "methods", "wmc", "lcom4",
                 "methods_without_members", "members", "cbo", "impl_merged"]


def strip_code(text):
    """Blanks comments, literals and preprocessor lines, keeping the line breaks."""
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group())
    return PREPROCESSOR_LINE.sub(blank, COMMENT_OR_LITERAL.sub(blank, text))


def strip_templates(text):
    """Removes template argument lists, from the innermost out."""
    stripped = TEMPLATE_ARGS.sub("", text)
    return strip_templates(stripped) if stripped != text else text


def qualified(name):
    """Splits a C++ name into its scopes, template arguments removed."""
    scopes = strip_templates(name).split("::")
    # lizard can print a scope twice, as in Io::Io::FileRecord: keep one.
    # The last name stays as written: a constructor repeats its class name.
    scope_list = [scope for index, scope in enumerate(scopes[:-1])
                  if index == 0 or scope != scopes[index - 1]]
    return scope_list + scopes[-1:]


def class_key(scopes):
    """Returns the full name of the class a scope list names. An Impl belongs to its outer class."""
    if len(scopes) > 1 and scopes[-1] == "Impl":
        scopes = scopes[:-1]
    return "::".join(scopes) if scopes else None


def is_impl(name):
    """True for a class named Impl inside another class."""
    scopes = qualified(name)
    return len(scopes) > 1 and scopes[-1] == "Impl"


def classify(header, parent):
    """Returns (kind, class name) for the scope a '{' opens inside a scope of kind parent."""
    if parent not in ("ns", "class"):
        return "other", None
    flat = " ".join(header.split())
    if re.search(r"\benum\b", flat):
        return "other", None
    match = CLASS_HEADER.search(flat)
    if match:
        return "class", match.group(1)
    namespace = re.search(r"\bnamespace\s+([\w:]+)", flat)
    if namespace:
        return "ns", namespace.group(1)
    if re.search(r"\bextern\b", flat):
        return "ns", None
    # A '(' before the brace: a function body, or a constructor's initializer list.
    if "(" in flat:
        return "func", None
    return "other", None


def data_members(declaration):
    """Returns (member names, declares an Impl) for a data member, else None."""
    text = " ".join(ACCESS_LABEL.sub(" ", declaration).split())
    if not text or NOT_DATA_MEMBER.search(text):
        return None
    declares_impl = re.search(r"\bImpl\b", text) is not None
    # The initializer is cut first, so a '(' inside it does not make a function.
    declarator = strip_templates(text.split("=", 1)[0]).replace("{}", "")
    if "(" in declarator:
        return None
    names = []
    for piece in declarator.split(","):
        head = re.split(r"\[|\{|(?<!:):(?!:)", piece)[0]
        found = IDENT.findall(head)
        if found:
            names.append(found[-1])
    return (names, declares_impl) if names else None


def scan_classes(file, code):
    """Returns the classes declared in one stripped file, with their data members."""
    newlines = [m.start() for m in re.finditer("\n", code)]
    # "scope" is the list of namespace and class names around a scope.
    frames = [{"kind": "ns", "text": "", "scope": []}]
    classes = []
    pos = 0
    for boundary in BOUNDARY.finditer(code):
        frame = frames[-1]
        text = frame["text"] + code[pos:boundary.start()]
        pos = boundary.end()
        line = bisect.bisect_right(newlines, boundary.start()) + 1
        char = boundary.group()
        if char == ";":
            member = data_members(text) if frame["kind"] == "class" else None
            if member and not member[1]:
                frame["members"].update(member[0])
            frame["text"] = ""
        elif char == "{":
            kind, name = classify(text, frame["kind"])
            lead = text[:len(text) - len(text.lstrip())].count("\n")
            start = line - text.count("\n") + lead
            frame["text"] = ""
            scope = frame["scope"] + (qualified(name) if name and kind in ("ns", "class") else [])
            frames.append({"kind": kind, "text": "", "prefix": text, "name": name,
                           "scope": scope, "start": start, "members": set()})
        elif len(frames) > 1:
            child = frames.pop()
            parent = frames[-1]
            if child["kind"] == "class":
                classes.append({"file": file, "name": child["name"],
                                "key": class_key(child["scope"]),
                                "is_impl": is_impl(child["name"]),
                                "start": child["start"], "end": line,
                                "members": child["members"]})
                parent["text"] = ""
            else:
                parent["text"] = child["prefix"] + "{}" if child["kind"] == "other" else ""
    return classes


def merge_classes(classes):
    """Groups classes by key, an Impl joining its outer class. Returns (groups, ambiguous keys)."""
    groups = {}
    for cls in classes:
        group = groups.setdefault(cls["key"],
                                  {"parts": [], "members": set(), "impl_merged": False})
        group["parts"].append(cls)
        group["members"] |= cls["members"]
        group["impl_merged"] = group["impl_merged"] or cls["is_impl"]
    # Two classes with one simple name cannot be told apart by lizard's names.
    ambiguous = [key for key, group in groups.items()
                 if sum(not part["is_impl"] for part in group["parts"]) > 1]
    for key in ambiguous:
        del groups[key]
    return groups, ambiguous


def resolve_key(key, known):
    """Returns the known class a lizard scope names. lizard glues an export macro to the name."""
    if key is None or key in known:
        return key
    head, _, last = key.rpartition("::")
    for candidate in known:
        cand_head, _, cand_last = candidate.rpartition("::")
        if (cand_head == head and last != cand_last and last.endswith(cand_last)
                and EXPORT_PREFIX.fullmatch(last[:-len(cand_last)])):
            return candidate
    return key


def class_methods(funs, known):
    """Returns {class key: methods} for the lizard functions whose name has a class scope."""
    methods = defaultdict(list)
    for fun in funs:
        scopes = qualified(fun.name)
        if len(scopes) > 1:
            key = resolve_key(class_key(scopes[:-1]), known)
            methods[key].append({
                "name": scopes[-1], "ccn": fun.cyclomatic_complexity,
                "file": rel(fun.filename), "start": fun.start_line, "end": fun.end_line})
    return methods


def is_constructor(name, key):
    """True for a constructor, a destructor, or the constructor of an Impl."""
    return name in (key.rpartition("::")[2], "Impl") or name.startswith("~")


def body_lines(code_lines, span):
    """Returns the stripped lines of a function or a class, from its file and line range."""
    return code_lines.get(span["file"], [])[span["start"] - 1:span["end"]]


def member_usage(body, members, method_names):
    """Returns (members the body touches, methods it calls on its own object)."""
    touched, called = set(), set()
    for match in IDENT.finditer(body):
        name = match.group()
        before = body[max(0, match.start() - 32):match.start()].rstrip()
        # Another object's member, or a name in another scope, is not this class's.
        if before.endswith(("::", ".")):
            continue
        if before.endswith("->") and not OWN_ARROW.search(before):
            continue
        if name in members:
            touched.add(name)
        if name in method_names and CALL.match(body, match.end()):
            called.add(name)
    return touched, called


def cohesion(key, owned, members, code_lines):
    """Returns (LCOM4, methods touching no member) over the methods that are not constructors."""
    nodes = [m for m in owned if not is_constructor(m["name"], key)]
    by_name = defaultdict(list)
    for index, method in enumerate(nodes):
        by_name[method["name"]].append(index)
    parent = {}

    def find(node):
        parent.setdefault(node, node)
        while parent[node] != node:
            parent[node] = parent[parent[node]]
            node = parent[node]
        return node

    lonely = 0
    for index, method in enumerate(nodes):
        touched, called = member_usage("\n".join(body_lines(code_lines, method)),
                                       members, by_name)
        if not touched:
            lonely += 1
        for name in touched:
            parent[find(("m", index))] = find(("v", name))
        for name in called:
            for other in by_name[name]:
                parent[find(("m", index))] = find(("m", other))
    roots = {find(("m", index)) for index in range(len(nodes))}
    return len(roots), lonely


def resolve_scopes(scopes, owner, by_simple):
    """Returns the known classes a written scope chain can name, seen from the owner class."""
    name = scopes[-1]
    candidates = by_simple.get(name, set())
    if len(scopes) > 1:
        written = "::".join(scopes)
        return {key for key in candidates if key == written or key.endswith("::" + written)}
    owner_parts = owner.split("::")
    # Innermost scope first: the owner class, its namespaces, then the global scope.
    for depth in range(len(owner_parts), -1, -1):
        scope = "::".join(owner_parts[:depth])
        full = f"{scope}::{name}" if scope else name
        if full in candidates:
            return {full}
    # A using-declaration, or a scope the scan does not track: any class of that name.
    return set(candidates)


def referenced_keys(text, owner, by_simple):
    """Returns the known classes a stripped text names, resolved from the owner's scopes."""
    found = set()
    for match in QUALIFIED_NAME.finditer(text):
        before = text[max(0, match.start() - 32):match.start()].rstrip()
        # After "X<...>::" or a leading "::", the scope is one the scan cannot read.
        if before.endswith("::"):
            continue
        scopes = [scope.strip() for scope in match.group().split("::")]
        for end in range(1, len(scopes) + 1):
            found |= resolve_scopes(scopes[:end], owner, by_simple)
    return found


def class_rows(groups, methods, code_lines):
    """Returns one row per class: WMC, LCOM4 and CBO. An Impl counts in its outer class."""
    by_simple = defaultdict(set)
    for key in groups:
        by_simple[key.rpartition("::")[2]].add(key)
    rows = []
    for key in sorted(groups):
        group = groups[key]
        owned = methods.get(key, [])
        primary = next((part for part in group["parts"] if not part["is_impl"]),
                       group["parts"][0])
        components, lonely = cohesion(key, owned, group["members"], code_lines)
        # CBO: the classes the text names, resolved as C++ resolves the written scopes.
        coupled = set()
        for part in group["parts"]:
            coupled |= referenced_keys("\n".join(body_lines(code_lines, part)), key, by_simple)
        for method in owned:
            coupled |= referenced_keys("\n".join(body_lines(code_lines, method)), key, by_simple)
        coupled -= {key}
        rows.append({
            "file": primary["file"], "class": key,
            "start_line": primary["start"], "end_line": primary["end"],
            "methods": len(owned), "wmc": sum(m["ccn"] for m in owned),
            "lcom4": components, "methods_without_members": lonely,
            "members": len(group["members"]), "cbo": len(coupled),
            "impl_merged": group["impl_merged"],
        })
    return rows


def summarize_classes(rows, matched, ambiguous):
    return {
        "count": len(rows),
        "impl_merged": sum(row["impl_merged"] for row in rows),
        "ambiguous_skipped": ambiguous,
        "methods_matched": matched,
    }


def class_section(info, rows, top):
    """Returns the Classes part of summary.md."""
    header = ["| LCOM4 | WMC | CBO | Methods | Members | Class | Location |",
              "|---|---|---|---|---|---|---|"]

    def table(title, ranked):
        return ["", f"### {title}", "", *header,
                *[f"| {r['lcom4']} | {r['wmc']} | {r['cbo']} | {r['methods']} | "
                  f"{r['members']} | `{r['class']}` | {r['file']}:{r['start_line']} |"
                  for r in ranked]]

    by_cohesion = sorted(rows, key=lambda r: (r["lcom4"], r["wmc"]), reverse=True)[:top]
    by_coupling = sorted(rows, key=lambda r: r["cbo"], reverse=True)[:top]
    return ["", "## Classes", "",
            f"{info['count']} classes, {info['impl_merged']} merged with their Impl. "
            f"Ambiguous names skipped: {info['ambiguous_skipped']}. "
            f"Functions matched to a class: {info['methods_matched']}.", "",
            *table(f"Top {top} by LCOM4, then WMC", by_cohesion),
            *table(f"Top {top} by CBO", by_coupling)]


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


def summarize(rows, infos, blocks, duplicates, bools, unmatched, coverage_source, churn_on):
    return {
        "lizard": version("lizard"),
        "coverage": {
            "source": str(coverage_source) if coverage_source else None,
            "functions_without_data": (sum(row["coverage_pct"] is None for row in rows)
                                       if coverage_source else None),
        },
        "churn": {
            "enabled": churn_on,
            "functions_without_history": (sum(row["churn_commits"] is None for row in rows)
                                          if churn_on else None),
        },
        "files": len(infos),
        "functions": len(rows),
        "nloc_in_functions": sum(row["nloc"] for row in rows),
        "nloc_in_files": sum(info.nloc for info in infos),
        "average": {c: number(mean(present(rows, c))) for c in NUMERIC_COLUMNS},
        "maximum": {c: number(max(present(rows, c), default=0)) for c in NUMERIC_COLUMNS},
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


def escape_cell(text):
    """Escapes the pipes that would end a markdown table cell early."""
    return str(text).replace("|", "\\|")


def limit_label(column):
    """Returns the table label of a limited column: its meaning, or its short label."""
    return MEANINGS.get(column) or LIMIT_LABELS[column]


def count_limits(limits, rows):
    """Returns, per column, its alert and limit, and how many rows are above each."""
    return {column: {"alert": alert, "limit": limit,
                     "above_alert": sum(v > alert for v in present(rows, column)),
                     "above_limit": sum(v > limit for v in present(rows, column))}
            for column, (alert, limit) in limits.items()}


def is_heavy(row):
    """True for a function above the heavy line in CCN or in lines."""
    return row["ccn"] > HEAVY_CCN or row["nloc"] > HEAVY_NLOC


def coverage_breaches(row):
    """Returns the ways a heavy function misses the coverage prerequisite. Empty without data."""
    if not is_heavy(row) or row.get("coverage_pct") is None:
        return []
    breaches = []
    if row["coverage_pct"] < MIN_COVERAGE_PCT:
        breaches.append(f"coverage {row['coverage_pct']} < {MIN_COVERAGE_PCT}")
    if row["branch_gap_lines"] > MAX_BRANCH_GAP_LINES:
        breaches.append(f"branch gap {row['branch_gap_lines']} > {MAX_BRANCH_GAP_LINES}")
    return breaches


def coverage_limits(rows):
    """Counts the heavy functions with coverage data, and those that miss the prerequisite."""
    measured = [row for row in rows if is_heavy(row) and row.get("coverage_pct") is not None]
    return {"heavy": len(measured),
            "below_prerequisite": sum(bool(coverage_breaches(row)) for row in measured)}


def limit_breaches(row, limits):
    """Returns the values of a row above their limit, as 'column value > limit' texts."""
    return [f"{column} {row[column]} > {limit}" for column, (_, limit) in limits.items()
            if row.get(column) is not None and row[column] > limit]


def breach_entries(hits):
    """Keeps the (breaches, file, line, name) hits that have a breach, most breaches first."""
    kept = [hit for hit in hits if hit[0]]
    return sorted(kept, key=lambda hit: (-len(hit[0]), hit[1], hit[2]))


def breach_row(hit):
    """Formats a (breaches, file, line, name) hit as a table row."""
    texts, file, line, name = hit
    return f"| {', '.join(texts)} | {name} | {file}:{line} |"


def beyond_block(title, header, body):
    """Returns a '### title' block with its table, or 'None.' when the body is empty."""
    if not body:
        return ["", f"### {title}", "", "None."]
    return ["", f"### {title}", "", header, "|" + "---|" * (header.count("|") - 1), *body]


def write_markdown(path, summary, rows, files, top, coupling, class_table):
    limits = summary["limits"]
    cycles = summary["includes"]["cycles"]
    coverage = limits["coverage"]
    lines = [
        "# ntfs-browser code metrics",
        "",
        f"lizard {summary['lizard']}: {summary['functions']} functions in "
        f"{summary['files']} files, {summary['nloc_in_functions']} lines in functions.",
        "",
        "## Limits",
        "",
        "| Indicator | Alert | Limit | Above alert | Above limit |",
        "|---|---|---|---|---|",
    ]
    for group in ("functions", "classes", "files", "includes"):
        lines += [f"| {escape_cell(limit_label(column))} | {count['alert']} | {count['limit']} | "
                  f"{count['above_alert']} | {count['above_limit']} |"
                  for column, count in limits[group].items()]
    lines += [
        "",
        f"Include cycles: {len(cycles)}. The limit is {CYCLE_LIMIT}, so any cycle is beyond it.",
        f"Heavy functions (CCN > {HEAVY_CCN} or NLOC > {HEAVY_NLOC}) need line coverage of "
        f"at least {MIN_COVERAGE_PCT}% and at most {MAX_BRANCH_GAP_LINES} branch gap lines.",
        (f"Coverage prerequisite: {coverage['below_prerequisite']} of {coverage['heavy']} "
         "heavy functions with coverage data miss it." if coverage
         else "Coverage prerequisite: no coverage data loaded."),
    ]
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
    lines += ["", "## Beyond the limit"]
    base_limits = {column: pair for column, pair in FUNCTION_LIMITS.items()
                   if column == "ccn" or column not in CCN_COLUMNS}
    function_hits = breach_entries(
        (limit_breaches(row, base_limits) + coverage_breaches(row), row["file"],
         row["start_line"], f"`{escape_cell(row['function'])}`") for row in rows)
    class_hits = breach_entries(
        (limit_breaches(row, CLASS_LIMITS), row["file"], row["start_line"], f"`{row['class']}`")
        for row in class_table)
    file_breaches = defaultdict(list)
    for row in files:
        file_breaches[row["file"]] += limit_breaches(row, FILE_LIMITS)
    for row in coupling:
        file_breaches[row["file"]] += limit_breaches(row, INCLUDE_LIMITS)
    file_hits = sorted(((texts, name) for name, texts in file_breaches.items() if texts),
                       key=lambda hit: (-len(hit[0]), hit[1]))
    lines += beyond_block("Functions", "| Breaches | Function | Location |",
                          [breach_row(hit) for hit in function_hits])
    lines += beyond_block("Classes", "| Breaches | Class | Location |",
                          [breach_row(hit) for hit in class_hits])
    lines += beyond_block("Files", "| Breaches | File |",
                          [f"| {', '.join(texts)} | {name} |" for texts, name in file_hits])
    lines += ["", "### Include cycles", ""]
    lines += [f"- {', '.join(group)}" for group in cycles] or ["None."]
    if coverage:
        heavy = sorted((row for row in rows if coverage_breaches(row)),
                       key=lambda row: (row["coverage_pct"], row["file"], row["start_line"]))
        lines += beyond_block(
            "Heavy functions below the coverage prerequisite",
            "| Coverage % | Branch gap lines | CCN | NLOC | Function | Location |",
            [f"| {row['coverage_pct']} | {row['branch_gap_lines']} | {row['ccn']} | "
             f"{row['nloc']} | `{escape_cell(row['function'])}` | "
             f"{row['file']}:{row['start_line']} |" for row in heavy])
    lines += [
        "",
        "## Averages and maxima per function",
        "",
        "| Metric | Average | Maximum |",
        "|---|---|---|",
    ]
    lines += [f"| {escape_cell(MEANINGS[column])} | {summary['average'][column]} | "
              f"{summary['maximum'][column]} |" for column in NUMERIC_COLUMNS]
    for column in TOP_METRICS:
        ranked = sorted(rows, key=lambda r: r.get(column) if r.get(column) is not None else -1,
                        reverse=True)[:top]
        lines += ["", f"## Top {top}: {MEANINGS[column]}", "",
                  f"| {escape_cell(MEANINGS[column])} | Function | Location |", "|---|---|---|"]
        lines += [f"| {row[column]} | `{escape_cell(row['function'])}` | "
                  f"{row['file']}:{row['start_line']} |" for row in ranked]
    if summary["coverage"]["source"]:
        risky = [row for row in rows
                 if row["ccn"] > HEAVY_CCN and row["coverage_pct"] is not None]
        risky.sort(key=lambda r: (r["coverage_pct"], -r["ccn"]))
        lines += ["", f"## Least covered functions with CCN > {HEAVY_CCN}", "",
                  f"Source: {summary['coverage']['source']}. Functions without coverage "
                  f"data: {summary['coverage']['functions_without_data']}.", "",
                  "| Coverage % | CCN | Uncovered lines | Function | Location |",
                  "|---|---|---|---|---|"]
        lines += [f"| {row['coverage_pct']} | {row['ccn']} | {row['uncovered_lines']} | "
                  f"`{escape_cell(row['function'])}` | {row['file']}:{row['start_line']} |"
                  for row in risky[:top]]
    if summary["churn"]["enabled"]:
        hot = [row for row in rows
               if row["ccn"] > HEAVY_CCN and row["churn_commits"] is not None]
        hot.sort(key=lambda r: (-r["churn_commits"], -r["ccn"]))
        lines += ["", f"## Hotspots: CCN > {HEAVY_CCN}, most changed first", "",
                  f"Functions without history: {summary['churn']['functions_without_history']}.",
                  "", "| Commits | Fixes | CCN | Last change | Function | Location |",
                  "|---|---|---|---|---|---|"]
        lines += [f"| {row['churn_commits']} | {row['churn_fixes']} | {row['ccn']} | "
                  f"{row['churn_last_change']} | `{escape_cell(row['function'])}` | "
                  f"{row['file']}:{row['start_line']} |" for row in hot[:top]]
    inc = summary["includes"]
    lines += ["", "## Include coupling", "",
              f"{inc['files']} files, {inc['edges']} include edges between them. "
              f"Ambiguous includes skipped: {inc['ambiguous_includes']}.", "",
              f"### Include cycles: {len(inc['cycles'])}", ""]
    lines += [f"- {', '.join(group)}" for group in inc["cycles"]] or ["None."]
    ranked = sorted(coupling, key=lambda r: -r["dependents"])[:top]
    lines += ["", f"### Top {top} by transitive dependents", "",
              "| Dependents | Depends on | Included by | Instability | File |",
              "|---|---|---|---|---|"]
    lines += [f"| {r['dependents']} | {r['depends_on']} | {r['includes_in']} | "
              f"{r['instability']} | {r['file']} |" for r in ranked]
    lines += class_section(summary["classes"], class_table, top)
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
    parser.add_argument("--coverage", type=Path, default=DEFAULT_COVERAGE,
                        help="coverage.py gaps-all.json (default: merged CI result)")
    parser.add_argument("--no-churn", action="store_true",
                        help="skip the git history (about 3 minutes)")
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

    coverage_source = None
    if args.coverage.is_file():
        add_coverage(rows, load_coverage(args.coverage))
        coverage_source = args.coverage
    else:
        print(f"no coverage file at {args.coverage}: coverage columns left empty")

    file_rows = [file_row(info) for info in infos]
    if not args.no_churn:
        add_function_churn(rows)
        add_file_churn(file_rows)

    args.out.mkdir(parents=True, exist_ok=True)
    write_csv(args.out / "functions.csv", rows, FUNCTION_COLUMNS)
    write_csv(args.out / "files.csv", file_rows, FILE_COLUMNS)
    write_duplicates(args.out / "duplicates.csv", blocks)
    sources = tracked_sources(args.paths)
    graph, ambiguous = include_graph(sources)
    coupling, cycles = coupling_rows(graph)
    write_csv(args.out / "includes.csv", coupling, INCLUDE_COLUMNS)

    code_lines, classes = {}, []
    for path in sources:
        code = strip_code((REPO / path).read_text(encoding="utf-8", errors="replace"))
        code_lines[path] = code.split("\n")
        classes += scan_classes(path, code)
    groups, ambiguous_classes = merge_classes(classes)
    methods = class_methods([fun for info in infos for fun in info.function_list], set(groups))
    class_table = class_rows(groups, methods, code_lines)
    write_csv(args.out / "classes.csv", class_table, CLASS_COLUMNS)

    summary = summarize(rows, infos, blocks, duplicates, bools, unmatched, coverage_source,
                        churn_on=not args.no_churn)
    summary["includes"] = {
        "files": len(sources),
        "edges": sum(len(targets) for targets in graph.values()),
        "ambiguous_includes": ambiguous,
        "cycles": cycles,
    }
    summary["classes"] = summarize_classes(
        class_table, sum(len(methods.get(row["class"], [])) for row in class_table),
        len(ambiguous_classes))
    summary["limits"] = {
        "functions": count_limits(FUNCTION_LIMITS, rows),
        "classes": count_limits(CLASS_LIMITS, class_table),
        "files": count_limits(FILE_LIMITS, file_rows),
        "includes": count_limits(INCLUDE_LIMITS, coupling),
        "coverage": coverage_limits(rows) if coverage_source else None,
    }
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8")
    write_markdown(args.out / "summary.md", summary, rows, file_rows, args.top, coupling,
                   class_table)
    print(f"{summary['functions']} functions in {summary['files']} files, "
          f"{len(blocks)} duplicated blocks -> {args.out}")


if __name__ == "__main__":
    main()
