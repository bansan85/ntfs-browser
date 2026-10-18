#!/usr/bin/env python3
"""Fail when a C++ ``friend`` declaration appears in the project sources.

Scans include/, src/ and NTFSLibTests/ for the token ``friend``, outside
comments and string or character literals. Prints one ``path:line`` per hit
on stderr and exits 1 when there is at least one hit.
"""

import argparse
import re
import sys
from pathlib import Path

SCANNED_DIRECTORIES = ("include", "src", "NTFSLibTests")
SCANNED_SUFFIXES = {".h", ".hpp", ".cpp", ".in"}
FRIEND = re.compile(r"\bfriend\b")

# One alternative per lexical element that can hide the word. The first
# alternative that matches at a position wins, so a quote inside a comment
# does not open a string and "//" inside a string does not open a comment.
LEXICAL_ELEMENT = re.compile(
    r"""
      (?P<raw>R"(?P<delim>[^()\\\s]{0,16})\(.*?\)(?P=delim)")
    | (?P<string>"(?:\\.|[^"\\\n])*")
    | (?P<char>'(?:\\.|[^'\\\n])*')
    | (?P<line_comment>//(?:\\\n|[^\n])*)
    | (?P<block_comment>/\*.*?\*/)
    """,
    re.DOTALL | re.VERBOSE,
)


def blank_out(match: re.Match[str]) -> str:
    """Replaces a match by spaces, keeping its newlines, so lines stay put."""
    return re.sub(r"[^\n]", " ", match.group(0))


def friend_lines(text: str) -> list[int]:
    """Line numbers (1-based) of every ``friend`` token outside comments and literals."""
    code = LEXICAL_ELEMENT.sub(blank_out, text)
    return [
        number
        for number, line in enumerate(code.splitlines(), start=1)
        if FRIEND.search(line)
    ]


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
                f"{relative}:{line}: friend declaration"
                for line in friend_lines(text)
            )
    return hits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (default: two levels above this script)",
    )
    arguments = parser.parse_args()
    hits = scan(arguments.root)
    for hit in hits:
        print(hit, file=sys.stderr)
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
