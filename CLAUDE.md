# CLAUDE.md

This file guides Claude Code when working with code in this repository.

## Project overview

`ntfs-browser` is a C++20, Windows-only library. It parses NTFS volumes directly: raw disk and MFT structures, not filesystem APIs. It is a modernized, read-only rewrite of the old CodeProject "An NTFS Parser Lib" (BSD-3c).

## Build

The build requires Windows and MSVC only. It uses `<windows.h>` and `<tchar.h>`; MFT/BPB parsing assumes Win32. It requires submodules (`3rdparty/gsl`, `3rdparty/Catch2`). Clone with `--recurse-submodules`, or run `git submodule update --init --recursive`.

```
cmake -S . -B build
cmake --build build --config Debug
```

An existing configured `build/` directory (Visual Studio generator) is already present in this repo. You can also open `build/NtfsBrowser.slnx` in Visual Studio.

`BUILD_SHARED_LIBS` (default OFF) selects a static or shared `NtfsBrowser` lib. CI (`.github/workflows/cmake.yml`) builds both Debug and Release, and both static and shared, on `windows-latest`.

## Tests

Tests use Catch2 (vendored under `3rdparty/Catch2`). CTest registers them via `catch_discover_tests`.

```
cmake --build build --config Debug --target NtfsBrowserTests
ctest --test-dir build -C Debug
```

Run a single test case by name. Catch2's tag/name filter passes through ctest with `-R`. Or invoke the test binary directly:
```
build/NTFSLibTests/unit-tests/Debug/NtfsBrowserTests.exe "<test name or tag>"
```

Test sources live in [NTFSLibTests/unit-tests/](NTFSLibTests/unit-tests/). Tests must not touch a real disk. They build synthetic NTFS images in memory ([fake-ntfs-image.h](NTFSLibTests/unit-tests/fake-ntfs-image.h)), using the library's own on-disk struct layouts from `src/data` and `src/attr`. They serve those images through fake `IDiskReader` implementations: [memory-disk-reader.h](NTFSLibTests/unit-tests/memory-disk-reader.h) (random-access, whole buffer in memory) or [sequential-disk-reader.h](NTFSLibTests/unit-tests/sequential-disk-reader.h) (offset-ignoring, chunk-at-a-time). Prefer extending these fakes over adding new test scaffolding.

`NTFSLibTests/ntfsattr`, `ntfsdir`, `ntfsdump`, `ntfsundel` are older sample/demo apps. `ntfsdir` is the simplest: it opens a volume, parses the root `FileRecord`, walks down to a path, and traverses entries.

## Formatting and linting

CI enforces `clang-format` (config in [.clang-format](.clang-format)) and `cmake-format` (via the `cmakelang` pip package). See [.github/workflows/format.yml](.github/workflows/format.yml): non-main-branch PRs fail the build if formatting changes. Run before committing:
```
bash ./.github/scripts/format.sh
```
`.clang-tidy` enables nearly all checks (`Checks: '*'`, minus a short exclusion list), with `WarningsAsErrors: '*'`.

## Writing style

Use short sentences: in code comments and in documentation.

Use RFC 2119 keywords (MUST, MUST NOT, SHOULD, MAY, ...) to state an obligation.

A comment MUST clarify something the code cannot say on its own. A comment MUST NOT narrate what the code already shows.

Every public function in `include/ntfs-browser/*.h` gets a purpose comment. One line, if the function name and every parameter name are self-explanatory. Several lines, if the purpose itself is not obvious. A single parameter can get its own comment, if only that parameter's effect is unclear.

A private method — declared with no public header, or not defined inline — gets the same treatment, in the `.cpp` file where it is implemented.

Every constant (`inline constexpr` in a header, `constexpr` in a `.cpp`) gets a comment: what it is for, and why it has that value.

A `TEST_CASE` (or `SECTION`) MUST NOT get a comment above it. Catch2's own name string is already the description.

Code that is not obvious to a developer reading it gets one comment line, directly on the line before it. This is the only other case where a comment belongs outside a function/constant purpose comment.

A boolean condition needs a comment above it once it combines at least four distinct elements — variables, `sizeof(...)` calls, casts, or literals — across more than one kind of check (eg. bounds arithmetic, a width cast, a sentinel/terminator comparison). Fewer than four elements does not meet this bar, even if the condition spans multiple lines for formatting reasons only. The comment states what the condition MEANS (the invariant it enforces), not what each term computes. One line normally; two lines if the condition's own complexity or length needs it.

Skip the comment entirely if a trace/logging call inside the same branch already states, in its own message, what the condition rejects.

Commit messages follow the `git-commit` skill.

## Architecture

### Strategy-templated core

Almost every core class is templated on `Strategy` ([include/ntfs-browser/strategy.h](include/ntfs-browser/strategy.h)): `Strategy::NO_CACHE` or `Strategy::FULL_CACHE`. This propagates through `NtfsVolume<S>` → `FileReader<S>` → `FileRecord<S>` → `AttrBase<S>` and all `Attr*<S>` subclasses. `FULL_CACHE` retains every read cluster in a map for reuse. `NO_CACHE` re-reads and returns views into a short-lived buffer instead. Attributes under `NO_CACHE` hold raw pointers or spans into that buffer. Callers MUST NOT assume attribute data outlives the next read. Pick the strategy that matches lifetime needs when writing code that touches these templates.

### Read path / object graph

- `IDiskReader` ([include/ntfs-browser/disk-reader.h](include/ntfs-browser/disk-reader.h)) abstracts "get raw bytes from a backing store" behind `Open()`/`ReadInto()`. `Win32DiskReader` is the production implementation: a real disk/device handle, or a plain file treated the same way via `CreateFileW`. Tests substitute `MemoryDiskReader` or `SequentialDiskReader`.
- `NtfsVolume<S>` ([include/ntfs-browser/ntfs-volume.h](include/ntfs-browser/ntfs-volume.h)) owns a `FileReader<S>`. It can be constructed from a drive letter, an arbitrary path (device or image file), or an already-open `IDiskReader` (for tests/fakes). It parses the boot sector (BPB) to learn sector, cluster, file-record, and index-block sizes. It then locates and parses the `$MFT` file record itself (`mft_record_`), to resolve further file records through possibly-fragmented `$MFT` data runs.
- `FileRecord<S>` ([include/ntfs-browser/file-record.h](include/ntfs-browser/file-record.h)) represents one MFT file record. `ParseFileRecord(fileRef)` reads it. `ParseAttrs()` walks and instantiates its attributes; an optional `Mask` can skip unwanted attribute types. Set the mask before parsing, to avoid wasted work. Directory traversal (`TraverseSubEntries`, `FindSubEntry`) walks `$INDEX_ROOT`/`$INDEX_ALLOCATION` B-tree index entries. This includes index entries that only reach a file via `$ATTRIBUTE_LIST`: a record's attributes can be split across an extension record.
- `AttrBase<S>` ([include/ntfs-browser/attr-base.h](include/ntfs-browser/attr-base.h)) is the base for all attribute wrappers (`AttrResident`, `AttrNonResident`, and the concrete `Attr*` types in `src/`, e.g. `attr-file-name`, `attr-index-root`, `attr-std-info`, `attr-list` for `$ATTRIBUTE_LIST`). Resident and non-resident attributes differ in how `GetData()`/`ReadData()` fetch bytes: inline in the record, versus following data runs through clusters.

### Directory structure

- `include/ntfs-browser/` — public API headers. These are what consumers of the library include.
- `src/` — implementation, plus internal-only headers not exposed publicly: `src/data/` (on-disk struct layouts: boot sector BPB, file record header, index block/entry, run entry), `src/attr/` (attribute header/type layouts), `src/flag/` (bitflag enums for filename/index-entry/std-info attributes). Tests include from `src/` directly (see `NTFSLibTests/unit-tests/CMakeLists.txt`), to reuse these on-disk struct layouts when building fake images, rather than duplicating byte offsets.
- `NTFSLibTests/` — Catch2 unit tests, plus the older MFC-based sample/demo apps.

### Callbacks

Two callback mechanisms exist. `AttrRawCallback` (installed via `InstallAttrRawCB` on `NtfsVolume`/`FileRecord`) fires during raw attribute parsing; it can veto or discard an attribute before it's wrapped. `ATTRS_CALLBACK`/`SUBENTRY_CALLBACK` are post-parse traversal callbacks (`TraverseAttrs`, `TraverseSubEntries`) for consumer code, matching the style used in the sample apps.
