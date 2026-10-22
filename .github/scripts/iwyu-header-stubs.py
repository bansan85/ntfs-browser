#!/usr/bin/env python3
"""Let include-what-you-use analyse the headers that have no .cpp.

IWYU only checks a header as the associated header of a main file with the
same stem. This script writes, for each header whose stem matches no source
of compile_commands.json, a stub ``<stem>.cpp`` that includes only that
header, and adds the stub to a copy of the database. Running IWYU over the
stubs then analyses each header once, on its own.

Usage: iwyu-header-stubs.py <build dir> [<repo root>]

Writes <build dir>/iwyu-stubs/ and <build dir>/iwyu-db/compile_commands.json.
"""

import argparse
import json
import shlex
import sys
from pathlib import Path

HEADER_DIRECTORIES = ("include", "src", "NTFSLibTests")
HEADER_SUFFIXES = {".h", ".hpp"}
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx"}
STUB_DIRECTORY = "iwyu-stubs"
DATABASE_DIRECTORY = "iwyu-db"


def entry_arguments(entry: dict) -> list[str]:
    """The compiler command of a database entry, as a list of arguments."""
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def entry_source(entry: dict) -> Path:
    """The absolute path of the file a database entry compiles."""
    path = Path(entry["file"])
    return path if path.is_absolute() else Path(entry["directory"]) / path


def include_roots(arguments: list[str], directory: str) -> list[Path]:
    """The ``-I`` and ``-isystem`` directories of a command, in search order."""
    roots: list[Path] = []
    for position, argument in enumerate(arguments):
        value = None
        if argument in ("-I", "-isystem") and position + 1 < len(arguments):
            value = arguments[position + 1]
        elif argument.startswith("-I") and len(argument) > 2:
            value = argument[2:]
        elif argument.startswith("-isystem") and len(argument) > 8:
            value = argument[8:]
        if value is not None:
            path = Path(value)
            roots.append(path if path.is_absolute() else Path(directory) / path)
    return roots


def spell_include(header: Path, roots: list[Path]) -> str:
    """How code spells ``header``: relative to the first include root holding it."""
    for root in roots:
        try:
            return header.relative_to(root).as_posix()
        except ValueError:
            continue
    return header.as_posix()


def find_sibling(header: Path, sources: list[tuple[Path, dict]]) -> dict:
    """The entry of the source nearest to ``header``: same folder, else a parent."""
    for folder in [header.parent, *header.parent.parents]:
        for source, entry in sources:
            if source.parent == folder:
                return entry
    return sources[0][1]


def make_stub_entry(
    sibling: dict, sibling_source: Path, stub: Path, header: Path
) -> tuple[dict, str]:
    """A database entry for ``stub``, with the compile flags of ``sibling``."""
    arguments = entry_arguments(sibling)
    directory = sibling["directory"]
    spelled = spell_include(header, include_roots(arguments, directory))
    replaced = [
        stub.as_posix() if Path(argument) == sibling_source else argument
        for argument in arguments
    ]
    entry = {"directory": directory, "arguments": replaced, "file": stub.as_posix()}
    return entry, spelled


def collect_headers(root: Path) -> list[Path]:
    headers: list[Path] = []
    for directory in HEADER_DIRECTORIES:
        base = root / directory
        if base.is_dir():
            headers.extend(
                path
                for path in sorted(base.rglob("*"))
                if path.suffix in HEADER_SUFFIXES and path.is_file()
            )
    return headers


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("root", type=Path, nargs="?", default=Path.cwd())
    args = parser.parse_args()

    build = args.build_dir.resolve()
    root = args.root.resolve()
    database = json.loads((build / "compile_commands.json").read_text("utf-8"))
    sources = [
        (entry_source(entry), entry)
        for entry in database
        if entry_source(entry).suffix in SOURCE_SUFFIXES
        and root in entry_source(entry).parents
    ]
    if not sources:
        print("no project source in compile_commands.json", file=sys.stderr)
        return 1

    covered = {source.stem for source, _ in sources}
    stub_root = build / STUB_DIRECTORY
    stubs: list[dict] = []
    for header in collect_headers(root):
        if header.stem in covered:
            continue
        sibling = find_sibling(header, sources)
        sibling_source = entry_source(sibling)
        stub = stub_root / header.relative_to(root).with_suffix(".cpp")
        entry, spelled = make_stub_entry(sibling, sibling_source, stub, header)
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text(f'#include "{spelled}"\n', encoding="utf-8")
        stubs.append(entry)

    merged = build / DATABASE_DIRECTORY
    merged.mkdir(parents=True, exist_ok=True)
    (merged / "compile_commands.json").write_text(
        json.dumps(database + stubs, indent=2), encoding="utf-8"
    )
    print(f"{len(stubs)} header stubs written to {stub_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
