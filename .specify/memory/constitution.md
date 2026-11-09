<!--
Sync Impact Report
Version change: 2.0.0 -> 3.0.0 (MAJOR: the public API leaves the Search sub-namespace for the root).
Awaiting maintainer approval.
Modified sections: Technical Constraints (the public types and functions, `Scan`, `Reader`, `Options`,
`Entry`, `Handle`, `SetLogSink()` and the rest, now live in the root `NTFS_BROWSER_NAMESPACE`. The
`Search` namespace no longer exists, in the public headers, in `src/search/` and in the tests. The
public headers moved from `include/ntfs-browser/search/` to `include/ntfs-browser/`: `scan.h`,
`types.h`, `reader.h`, `reader-types.h` and `log.h`). The domain namespaces and the five older root
names (`IDiskReader`, `NtfsVolume`, `IndexEntry`, `Filename`, `VolumeOptions`) stay.
Reason: only the public API remains in the public headers, so a namespace for it adds a prefix and
no information. Code that spelled `Search::Scan` is no longer compliant.
Previous: 1.5.0 -> 2.0.0 (MAJOR: the public surface shrinks to Search::Scan and Search::Reader).
Awaiting maintainer approval.
Modified sections: Technical Constraints (the public headers are `search/scan.h`, `search/types.h`,
`search/reader.h`, `search/reader-types.h` and `search/log.h` only. Every other header moved to `src/`
and is reached through a private include root. The spdlog adapter, `win-types.h`, tuning and bench
are internal. A shared library exports Scan, Reader and the log setter, and the tests-only symbols
only in a build with tests).
Reason: a consumer needs to list files, read them and receive the log. Nothing else is a promise.
A CMake check (`NtfsPublicApiCheck`) builds against the public headers alone.
Previous: 1.4.0 -> 1.5.0 (MINOR: materially expanded guidance).
Modified sections: Principle III (fake image path, namespaces of test code). Technical Constraints
(Search::Scan is the public entry point; the Linux and Windows-only executable lists; no MFC; the
header layout and namespaces of tests, tools and examples).
Reason: NtfsDir2 and NtfsMftList no longer exist. NtfsDir is built on Search::Scan. NtfsScanBench
lists and benchmarks a volume. NtfsFuzzer is the only Windows-only executable.
Templates: plan-template.md checked, no change needed.
Previous: 1.3.0 -> 1.4.0 (MINOR: materially expanded guidance).
Modified sections: Technical Constraints (public headers MUST NOT include spdlog, except
src/log/spdlog-adapter.h, an opt-in header-only adapter to Log::ISink).
Reason: consumers customize the log output, through sinks of their own or through their own spdlog
logger.
Previous: 1.2.0 -> 1.3.0 (MINOR: materially expanded guidance).
Modified sections: Technical Constraints (NtfsDir and NtfsMftList MUST build with GCC on Linux).
Reason: NtfsDir and NtfsMftList ran on Linux. Both names changed later, see 1.5.0.
Previous: 1.1.0 -> 1.2.0 (MINOR: new rule, no function body in a header for a Strategy-only
template).
Previous: 1.0.0 -> 1.1.0 (MINOR: new MUST, no `friend` declaration).
-->

# NtfsBrowser Constitution

## Core Principles

### I. Direct MFT Access for Performance

NtfsBrowser MUST read volumes by parsing raw disk bytes and the MFT directly.
The library MUST NOT use filesystem APIs to enumerate or read volume content.
Tools that compare against the operating system's view, such as NtfsCompare, are exempt.
Full MFT scans (`MftTree`) SHOULD run on a `NoCache` volume, so the whole `$MFT` is never held in memory.
A change that claims a speed gain MUST show a measurement.

Rationale: reading the MFT directly is the reason this library exists. Going through
the OS removes that advantage.

### II. Corrupt Data Is Untrusted Input

Every byte read from a volume, device or image is untrusted.
The library MUST reject a damaged item (file record, index block, cluster run, attribute list)
without crashing.
A crash, access violation or undefined behaviour MUST NOT escape the library.
Rejection MUST affect one item only: the enclosing scan or walk continues with the next item.
Salvaging partial data MUST be opt-in through `VolumeOptions::recover_errors`. Strict mode is the default.
Each crash found by fuzzing MUST become a scenario of `NtfsFuzzGen` (`NTFSLibTests/fuzz/generator`) as a regression input.
Its test MUST fail before the fix and pass after it.

Rationale: the disks this library reads are often damaged, and they are never trusted.

### III. Test Coverage on Synthetic and Corpus Images

Every behaviour MUST be covered by a Catch2 test.
Coverage MUST stay high. A change that adds code without tests MUST NOT merge.
Tests MUST NOT touch a real disk or device. They MUST use the synthetic images built by
`NTFSLibTests/tools/fake-image/` (`fake-ntfs-image.h`), or corpus image files reached through `RequireCorpusImage()` and `OpenCorpusImage()`.
Corpus traces come from `NtfsFuzzGen`, which writes them into the build tree. No trace is committed.
New test scaffolding SHOULD NOT be written when an existing fake disk reader can be extended.

Rationale: synthetic images cover the rules. Corpus images and traces catch what synthetic images miss.

### IV. Read-Only Operation

The library MUST NOT write to a volume, device or image. It MUST open them for reading only.
The library MUST NOT compress or encrypt data. Decompression and decryption are the only
transforms it applies.

Rationale: writing to a damaged or in-use volume destroys evidence and data.

### V. Scope: NTFS 3.0 and Later

The library MUST support NTFS 3.0 and later on-disk layouts.
Earlier NTFS versions are out of scope and MUST NOT be claimed as supported.
Attribute decompression (LZNT1) and EFS decryption (AES, 3DES, DESX) are in scope.
Each MUST remain compilable out through its feature option.

Rationale: a clear scope keeps the parser auditable against the on-disk specification.

## Technical Constraints

- The primary target is C++20 with MSVC on Windows. The library, the unit tests, NtfsAttr, NtfsDump,
  NtfsUndel, NtfsDir, NtfsScanBench, NtfsCompare, NtfsBench, NtfsFuzzGen and NtfsFuzzerAfl MUST also
  build with GCC on Linux. The build has no MFC.
- Windows-only executables MUST be guarded with `if(WIN32)` in CMake. NtfsFuzzer is the only one.
- `Scan` (`include/ntfs-browser/scan.h`) is the public entry point to list a folder or
  a volume, and `Reader` (`reader.h`) reads the files it found. NtfsDir and NtfsUndel are
  built on them. Their public headers MUST include only the standard library, `namespace.h`,
  `export.h` and each other.
- `include/`, `src/` and `NTFSLibTests/` MUST NOT contain a `friend` declaration. A class MUST get
  what it needs from another class through that class's methods, not through friendship.
  `.github/scripts/check-no-friend.py` enforces it in CI.
- The public headers in `include/ntfs-browser/` (`scan.h`, `types.h`, `reader.h`,
  `reader-types.h`, `log.h`) are the only consumer surface. They MUST NOT include spdlog, Crypto++,
  `<windows.h>`, any internal header or any feature macro. The public date is `Date`. A consumer
  receives the log through `SetLogSink()`. Every other header lives in `src/`, and the library
  target keeps `src/` private. Tests, tools and fuzzers get it through the `NtfsBrowserInternals`
  CMake target. `NtfsPublicApiCheck` builds against the public headers alone.
- Core classes that depend on cache behaviour MUST be templated on `Cache::Strategy`.
  Callers under `NoCache` MUST NOT assume attribute data outlives the next read.
- A template whose only parameter is a `Cache::Strategy` MUST NOT define a function in a header
  under `include/` or `src/`. A header declares it. Its code MUST live in one `.cpp`, which ends with
  the explicit instantiations for `NoCache` and `FullCache`. A template with another parameter is exempt.
  `.github/scripts/check-strategy-templates.py` enforces it in CI.
- `NtfsVolume`, `FileRecord` and `MftTree` MUST keep every data member and private method behind
  a private `Impl`.
- A symbol of a public header MUST carry `NTFS_BROWSER_EXPORT`.
  An internal symbol that tests, tools or fuzzers reach MUST carry `NTFS_BROWSER_EXPORT_TESTS_ONLY`.
- The public API (`Scan`, `Reader` and their types) MUST live in `NTFS_BROWSER_NAMESPACE` itself, with no
  sub-namespace. Every other type and function MUST live in a sub-namespace per domain. Code MUST NOT
  spell out the namespace name.
- A header that another test folder includes MUST live in
  `<module>/include/ntfs-browser/{test,tool,example}/<module>/`, and CMake never installs it.
  Test code lives in `Test::<Module>`, tools in `Tool::<Name>` and examples in `Example::<Name>`.
- The library and its logging are single-threaded. `SetLogSink()`, `Log::Configure()`,
  `Log::SetSinks()` and every log message MUST run on one thread.
- Every source file MUST be listed explicitly in its folder's `CMakeLists.txt`. Globbing MUST NOT be used.
- Every public function in `include/ntfs-browser/` and in `src/` headers MUST have a purpose comment.
  Every constant MUST have a comment that says what it is for and why it has its value.

## Development Workflow and Quality Gates

- Commits MUST follow Conventional Commits, without a scope, written in English.
  The title MUST start lowercase after the prefix, MUST NOT end with a period, and MUST NOT exceed
  50 characters including the prefix. The body wraps at 72 characters.
  Each commit MUST end with a `Co-Authored-By:` trailer.
- Code MUST be formatted before each commit with `uv run bash ./.github/scripts/format.sh`
  (clang-format 23.1.0 and gersemi).
- A bug fix MUST begin with a regression test that fails on the current code.
- A change to library code MUST pass the clang-tidy and cppcheck workflows before merge.
- A pull request MUST pass the CI matrix: Debug and Release, static and shared, Windows x64 and x86,
  Linux GCC and Clang, and each reduced feature configuration. `ctest` MUST pass on every leg.
- Obligations in comments and docs MUST use RFC 2119 keywords. A comment MUST say what the code
  cannot say on its own.

## Governance

This constitution supersedes other project practices.
`CLAUDE.md` is the runtime guidance for development. It MUST NOT contradict this file.
If the two disagree, the change that creates the conflict MUST fix one of them.

**Amendments**: an amendment is a pull request that edits this file and includes a Sync Impact Report.
The pull request MUST name each principle or section it changes, and the reason.
The project maintainer MUST approve it before merge.
An amendment that changes a gate MUST update the CI workflow or `CLAUDE.md` in the same pull request.

**Versioning**: MAJOR for removing or redefining a principle in a way that makes compliant code
non-compliant. MINOR for a new principle or section, or materially expanded guidance.
PATCH for wording, typo fixes and clarifications that change no obligation.

**Compliance**: every pull request review MUST verify principles I to V and the gates in
Development Workflow and Quality Gates. A violated MUST MUST be fixed, or justified in the
pull request description. Complexity beyond what a principle requires MUST be justified in the same way.

**Version**: 3.0.0 | **Ratified**: 2026-10-17 | **Last Amended**: 2026-11-09
