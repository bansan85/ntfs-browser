#!/usr/bin/env python3
"""Fail when a header defines a function of a Strategy-only template.

A Strategy-only template is a class, function or member function template
whose only template parameter is a ``Cache::Strategy``. It has exactly two
instantiations, so its code MUST live in one .cpp file that ends with the
explicit instantiations. Scans include/ and src/ for .h and .hpp files.
Prints one ``path:line: message`` per hit on stderr and exits 1 when there is
at least one hit.

``--self-test`` runs built-in samples instead and reads no repository file.
"""

import argparse
import re
import sys
from pathlib import Path

SCANNED_DIRECTORIES = ("include", "src")
SCANNED_SUFFIXES = {".h", ".hpp"}
MESSAGE = "function body in a Strategy-only template"

# One alternative per lexical element that can hide a brace or a keyword. The
# first alternative that matches at a position wins, so a quote inside a
# comment does not open a string and "//" inside a string does not open a
# comment. A quote that follows a letter or digit is a digit separator (4'096).
LEXICAL_ELEMENT = re.compile(
    r"""
      (?P<raw>R"(?P<delim>[^()\\\s]{0,16})\(.*?\)(?P=delim)")
    | (?P<string>"(?:\\.|[^"\\\n])*")
    | (?P<char>(?<![0-9A-Za-z])'(?:\\.|[^'\\\n])*')
    | (?P<line_comment>//(?:\\\n|[^\n])*)
    | (?P<block_comment>/\*.*?\*/)
    | (?P<preprocessor>^[ \t]*\#(?:\\\n|[^\n])*)
    """,
    re.DOTALL | re.MULTILINE | re.VERBOSE,
)
TEMPLATE_KEYWORD = re.compile(r"\btemplate\s*<")
ATTRIBUTE = re.compile(r"\[\[.*?\]\]", re.DOTALL)
CLASS_KEYWORD = re.compile(r"\b(?:class|struct|union)\b")
ENUM_KEYWORD = re.compile(r"\benum\b")
STRATEGY = re.compile(r"\bStrategy\b")


def blank_out(match: re.Match[str]) -> str:
    """Replaces a match by spaces, keeping its newlines, so lines stay put."""
    return re.sub(r"[^\n]", " ", match.group(0))


def blank_non_code(text: str) -> str:
    """Hides comments, literals and preprocessor lines from the scanner."""
    return LEXICAL_ELEMENT.sub(blank_out, text)


def match_close(code: str, start: int, opening: str, closing: str) -> int:
    """Index of the bracket that closes the one at start, or len(code)."""
    depth = 0
    for index in range(start, len(code)):
        character = code[index]
        if character == opening:
            depth += 1
        elif character == closing:
            depth -= 1
            if depth == 0:
                return index
    return len(code)


def template_parameters(code: str, keyword_end: int) -> tuple[list[str], int]:
    """Splits the parameter list after ``template <`` at its top-level commas.

    keyword_end is the index just after the ``<``. Returns the parameters and
    the index just after the closing ``>``.
    """
    depth = 1
    parameters: list[str] = []
    current: list[str] = []
    index = keyword_end
    while index < len(code) and depth > 0:
        character = code[index]
        if character == "<":
            depth += 1
        elif character == ">" and code[index - 1] != "-":
            depth -= 1
            if depth == 0:
                break
        if character == "," and depth == 1:
            parameters.append("".join(current).strip())
            current = []
        else:
            current.append(character)
        index += 1
    last = "".join(current).strip()
    if last:
        parameters.append(last)
    return parameters, index + 1


def is_strategy_only(parameters: list[str]) -> bool:
    """True for exactly one parameter, and that parameter names Strategy."""
    return len(parameters) == 1 and STRATEGY.search(parameters[0]) is not None


def declaration_end(code: str, start: int) -> int:
    """Index of the first ``;`` or ``{`` at parenthesis depth 0, or len(code)."""
    depth = 0
    for index in range(start, len(code)):
        character = code[index]
        if character in "([":
            depth += 1
        elif character in ")]":
            depth -= 1
        elif depth <= 0 and character in ";{":
            return index
    return len(code)


def line_of(code: str, index: int) -> int:
    return code.count("\n", 0, index) + 1


def scan_class_body(code: str, start: int, end: int, hits: list[int]) -> None:
    """Records the line of each function body between start and end.

    start is just after the ``{`` of a class and end is its closing ``}``. A
    member template with another parameter is exempt, so it is skipped whole.
    """
    index = start
    while index < end:
        template = TEMPLATE_KEYWORD.match(code, index, end)
        statement_start = index
        if template is None:
            next_template = TEMPLATE_KEYWORD.search(code, index, end)
            stop = declaration_end(code, index)
            if next_template is not None and next_template.start() < stop:
                # A template keyword inside the statement: let the next
                # iteration see it first.
                index = next_template.start()
                if index == statement_start:
                    index += 1
                continue
        else:
            parameters, after = template_parameters(code, template.end())
            stop = declaration_end(code, after)
            if not is_strategy_only(parameters):
                index = skip_declaration(code, stop, end)
                continue
            statement_start = after
        if stop >= end:
            return
        if code[stop] == ";":
            index = stop + 1
            continue
        head = ATTRIBUTE.sub("", code[statement_start:stop])
        close = match_close(code, stop, "{", "}")
        if "(" in head:
            hits.append(line_of(code, stop))
        elif CLASS_KEYWORD.search(head) and not ENUM_KEYWORD.search(head):
            scan_class_body(code, stop + 1, close, hits)
        index = close + 1


def skip_declaration(code: str, stop: int, end: int) -> int:
    """Index after the declaration whose ``;`` or ``{`` is at stop."""
    if stop >= end:
        return end
    if code[stop] == ";":
        return stop + 1
    return match_close(code, stop, "{", "}") + 1


def violation_lines(text: str) -> list[int]:
    """Line numbers (1-based) of every function body of a Strategy-only template."""
    code = blank_non_code(text)
    hits: list[int] = []
    cursor = 0
    while True:
        template = TEMPLATE_KEYWORD.search(code, cursor)
        if template is None:
            break
        parameters, after = template_parameters(code, template.end())
        stop = declaration_end(code, after)
        head = ATTRIBUTE.sub("", code[after:stop])
        # template <> is a full specialisation. It counts only when it names
        # a Strategy, as HeaderImpl<Cache::Strategy::NoCache> does.
        specialisation = not parameters and STRATEGY.search(head) is not None
        checked = is_strategy_only(parameters) or specialisation
        if stop >= len(code):
            break
        if code[stop] == ";":
            cursor = stop + 1
            continue
        close = match_close(code, stop, "{", "}")
        if checked:
            if "(" in head:
                hits.append(line_of(code, stop))
            elif CLASS_KEYWORD.search(head) and not ENUM_KEYWORD.search(head):
                scan_class_body(code, stop + 1, close, hits)
        cursor = close + 1
    return sorted(set(hits))


def scan(root: Path) -> list[str]:
    hits: list[str] = []
    for directory in SCANNED_DIRECTORIES:
        base = root / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SCANNED_SUFFIXES or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            relative = path.relative_to(root).as_posix()
            hits.extend(
                f"{relative}:{line}: {MESSAGE}" for line in violation_lines(text)
            )
    return hits


# (name, source, expected hit lines)
SELF_TEST_SAMPLES: list[tuple[str, str, list[int]]] = [
    (
        "class with an inline member body",
        "template <Cache::Strategy S>\n"
        "class X {\n"
        " public:\n"
        "  int Get() const { return 1; }\n"
        "};\n",
        [4],
    ),
    (
        "function with a body",
        "template <Cache::Strategy S>\nbool F(int a) {\n  return a > 0;\n}\n",
        [2],
    ),
    (
        "body only in a comment or a string",
        "template <Cache::Strategy S>\n"
        "class X {\n"
        "  // int Get() { return 1; }\n"
        '  const char* name = "void F() { }";\n'
        "  /* int G() { return 2; } */\n"
        "  void H();\n"
        "};\n",
        [],
    ),
    (
        "declarations, alias and static_assert",
        "template <Cache::Strategy S>\n"
        "class X {\n"
        " public:\n"
        "  X();\n"
        "  [[nodiscard]] bool Ok() const noexcept;\n"
        "  static constexpr int kSize{4};\n"
        "  std::vector<int> v_{};\n"
        "  void Fill(Options o = {});\n"
        "};\n"
        "template <Cache::Strategy S>\n"
        "using R = std::conditional_t<S == Cache::Strategy::FullCache, A, B>;\n"
        "template <Cache::Strategy S>\n"
        "[[nodiscard]] bool Attach(X<S>& x);\n"
        "static_assert(sizeof(int) == 4);\n",
        [],
    ),
    (
        "another template parameter is exempt",
        "template <class R, Cache::Strategy S>\n"
        "class Y {\n"
        "  int Get() const { return 1; }\n"
        "};\n"
        "template <class F>\n"
        "void Each(F f) { f(); }\n",
        [],
    ),
    (
        "defaulted and deleted members",
        "template <Cache::Strategy S>\n"
        "class X {\n"
        " public:\n"
        "  X() = default;\n"
        "  X(const X&) = delete;\n"
        "  ~X() = default;\n"
        "};\n",
        [],
    ),
    (
        "member function template over a Strategy with a body",
        "template <Cache::Strategy S>\n"
        "class X {\n"
        " public:\n"
        "  template <Cache::Strategy S2 = S>\n"
        "  int Read() const { return 1; }\n"
        "};\n",
        [5],
    ),
    (
        "member template with another parameter inside a Strategy class",
        "template <Cache::Strategy S>\n"
        "class X {\n"
        " public:\n"
        "  template <class F>\n"
        "  void Each(F f) const { f(); }\n"
        "  void Done();\n"
        "};\n",
        [],
    ),
    (
        "nested class with a body, a lambda initialiser",
        "template <Cache::Strategy S>\n"
        "class X {\n"
        "  struct Inner {\n"
        "    int Get() { return 1; }\n"
        "  };\n"
        "  std::function<int()> f_ = [] { return 2; };\n"
        "};\n",
        [4, 6],
    ),
    (
        "specialisation over a Strategy with a body",
        "template <>\n"
        "struct H<Cache::Strategy::NoCache> : Base {\n"
        "  int Get() const { return 1; }\n"
        "};\n"
        "template <>\n"
        "int G<int>() { return 1; }\n",
        [3],
    ),
    (
        "empty primary template and digit separator",
        "template <Cache::Strategy S>\n"
        "struct HeaderImpl {};\n"
        "inline constexpr auto big = 4'096;\n"
        "template <Cache::Strategy S>\n"
        "class X {\n"
        "  int Get() const { return 4'096; }\n"
        "};\n",
        [6],
    ),
]


def self_test() -> int:
    failures = 0
    for name, source, expected in SELF_TEST_SAMPLES:
        actual = violation_lines(source)
        if actual != expected:
            failures += 1
            print(
                f"self-test '{name}': expected lines {expected}, got {actual}",
                file=sys.stderr,
            )
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (default: two levels above this script)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the built-in samples and exit",
    )
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    hits = scan(arguments.root)
    for hit in hits:
        print(hit, file=sys.stderr)
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
