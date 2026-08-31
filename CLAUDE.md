# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

`ntfs-browser` is a C++20, Windows-only library for parsing NTFS volumes directly (reading raw disk/MFT structures, not going through the filesystem APIs). It's a modernized rewrite of the old CodeProject "An NTFS Parser Lib" (BSD-3c), read-only.

## Build

Windows + MSVC only (uses `<windows.h>`, `<tchar.h>`, MFT/BPB parsing assumes Win32). Requires submodules (`3rdparty/gsl`, `3rdparty/Catch2`) — clone with `--recurse-submodules` or run `git submodule update --init --recursive`.

```
cmake -S . -B build
cmake --build build --config Debug
```

An existing configured `build/` directory (Visual Studio generator) is already present in this repo; you can also open `build/NtfsBrowser.slnx` in Visual Studio.

`BUILD_SHARED_LIBS` (default OFF) controls static vs. shared `NtfsBrowser` lib. CI (`.github/workflows/cmake.yml`) builds both Debug/Release and both static/shared on `windows-latest`.

## Tests

Tests use Catch2 (vendored under `3rdparty/Catch2`) and are registered with CTest via `catch_discover_tests`.

```
cmake --build build --config Debug --target NtfsBrowserTests
ctest --test-dir build -C Debug
```

Run a single test case by name (Catch2 tag/name filter passes through ctest with `-R`, or invoke the test binary directly):
```
build/NTFSLibTests/unit-tests/Debug/NtfsBrowserTests.exe "<test name or tag>"
```

Test sources live in [NTFSLibTests/unit-tests/](NTFSLibTests/unit-tests/). Tests don't touch a real disk: they build synthetic NTFS images in memory ([fake-ntfs-image.h](NTFSLibTests/unit-tests/fake-ntfs-image.h)) using the library's own on-disk struct layouts from `src/data` and `src/attr`, and serve them through fake `IDiskReader` implementations — [memory-disk-reader.h](NTFSLibTests/unit-tests/memory-disk-reader.h) (random-access, whole buffer in memory) or [sequential-disk-reader.h](NTFSLibTests/unit-tests/sequential-disk-reader.h) (offset-ignoring, chunk-at-a-time). Prefer extending/reusing these fakes over adding new test scaffolding.

`NTFSLibTests/ntfsattr`, `ntfsdir`, `ntfsdump`, `ntfsundel` are older sample/demo apps (`ntfsdir` is the simplest, showing basic library usage: open a volume, parse the root `FileRecord`, walk down to a path, traverse entries).

## Formatting and linting

`clang-format` (config in [.clang-format](.clang-format)) and `cmake-format` (via the `cmakelang` pip package) are enforced in CI ([.github/workflows/format.yml](.github/workflows/format.yml), non-main-branch PRs fail the build if formatting changes). Run before committing:
```
bash ./.github/scripts/format.sh
```
`.clang-tidy` enables nearly all checks (`Checks: '*'` minus a short exclusion list) with `WarningsAsErrors: '*'`.

## Architecture

### Strategy-templated core

Almost every core class is templated on `Strategy` ([include/ntfs-browser/strategy.h](include/ntfs-browser/strategy.h)): `Strategy::NO_CACHE` or `Strategy::FULL_CACHE`. This propagates through `NtfsVolume<S>` → `FileReader<S>` → `FileRecord<S>` → `AttrBase<S>` and all `Attr*<S>` subclasses. `FULL_CACHE` retains every read cluster in a map for reuse; `NO_CACHE` re-reads and returns views into a short-lived buffer (attributes under `NO_CACHE` hold raw pointers/spans into that buffer — never assume attribute data outlives the next read). Pick the strategy that matches lifetime needs when writing code that touches these templates.

### Read path / object graph

- `IDiskReader` ([include/ntfs-browser/disk-reader.h](include/ntfs-browser/disk-reader.h)) abstracts "get raw bytes from a backing store" behind `Open()`/`ReadInto()`. `Win32DiskReader` is the production implementation (real disk/device handle, or a plain file treated the same way via `CreateFileW`); tests substitute `MemoryDiskReader` / `SequentialDiskReader`.
- `NtfsVolume<S>` ([include/ntfs-browser/ntfs-volume.h](include/ntfs-browser/ntfs-volume.h)) owns a `FileReader<S>` and can be constructed from a drive letter, an arbitrary path (device or image file), or an already-open `IDiskReader` (for tests/fakes). It parses the boot sector (BPB) to learn sector/cluster/file-record/index-block sizes, then locates and parses the `$MFT` file record itself (`mft_record_`) to resolve further file records through possibly-fragmented `$MFT` data runs.
- `FileRecord<S>` ([include/ntfs-browser/file-record.h](include/ntfs-browser/file-record.h)) represents one MFT file record. `ParseFileRecord(fileRef)` reads it, `ParseAttrs()` walks and instantiates its attributes (respecting an optional `Mask` to skip unwanted attribute types — set this before parsing to avoid wasted work). Directory traversal (`TraverseSubEntries`, `FindSubEntry`) walks `$INDEX_ROOT`/`$INDEX_ALLOCATION` B-tree index entries, including index entries that only reach a file via `$ATTRIBUTE_LIST` (a record's attributes can be split across an extension record).
- `AttrBase<S>` ([include/ntfs-browser/attr-base.h](include/ntfs-browser/attr-base.h)) is the base for all attribute wrappers (`AttrResident`, `AttrNonResident`, and the concrete `Attr*` types in `src/`, e.g. `attr-file-name`, `attr-index-root`, `attr-std-info`, `attr-list` for `$ATTRIBUTE_LIST`). Resident vs. non-resident attributes differ in how `GetData()`/`ReadData()` fetch bytes (inline in the record vs. following data runs through clusters).

### Directory structure

- `include/ntfs-browser/` — public API headers (what consumers of the library include).
- `src/` — implementation, plus internal-only headers not exposed publicly: `src/data/` (on-disk struct layouts — boot sector BPB, file record header, index block/entry, run entry), `src/attr/` (attribute header/type layouts), `src/flag/` (bitflag enums for filename/index-entry/std-info attributes). Tests include from `src/` directly (see `NTFSLibTests/unit-tests/CMakeLists.txt`) specifically to reuse these on-disk struct layouts when building fake images, rather than duplicating byte offsets.
- `NTFSLibTests/` — Catch2 unit tests plus the older MFC-based sample/demo apps.

### Callbacks

Two callback mechanisms exist: `AttrRawCallback` (installed via `InstallAttrRawCB` on `NtfsVolume`/`FileRecord`) fires during raw attribute parsing and can veto/discard an attribute before it's wrapped; `ATTRS_CALLBACK`/`SUBENTRY_CALLBACK` are post-parse traversal callbacks (`TraverseAttrs`, `TraverseSubEntries`) for consumer code, matching the style used in the sample apps.
