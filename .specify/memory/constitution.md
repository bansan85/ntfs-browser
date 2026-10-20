<!--
Sync Impact Report
Version change: 1.3.0 -> 1.4.0 (MINOR: materially expanded guidance). Awaiting maintainer approval.
Modified sections: Technical Constraints (public headers MUST NOT include spdlog, except
include/ntfs-browser/log/spdlog-adapter.h, an opt-in header-only adapter to Log::ISink).
Reason: consumers customize the log output, through sinks of their own or through their own spdlog
logger.
Previous: 1.2.0 -> 1.3.0 (MINOR: materially expanded guidance).
Modified sections: Technical Constraints (NtfsDir and NtfsMftList MUST build with GCC on Linux;
the Windows-only list names NtfsDir2, NtfsAttr, NtfsDump, NtfsUndel and NtfsFuzzer).
Reason: NtfsDir and NtfsMftList run on Linux.
Templates: plan-template.md checked, no change needed.
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
Each crash found by fuzzing MUST be saved in `NTFSLibTests/fuzz/data/` as a regression input.
Its test MUST fail before the fix and pass after it.

Rationale: the disks this library reads are often damaged, and they are never trusted.

### III. Test Coverage on Synthetic and Corpus Images

Every behaviour MUST be covered by a Catch2 test.
Coverage MUST stay high. A change that adds code without tests MUST NOT merge.
Tests MUST NOT touch a real disk or device. They MUST use the synthetic images in
`fake-ntfs-image.h`, or corpus image files reached through `RequireCorpusImage()` and `OpenCorpusImage()`.
Corpus traces in `NTFSLibTests/fuzz/data/` MUST be re-recorded after any change to the read path.
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

- The primary target is C++20 with MSVC on Windows. The library, the unit tests, NtfsDir,
  NtfsMftList, NtfsCompare and NtfsFuzzerAfl MUST also build with GCC on Linux.
- Windows-only executables MUST be guarded with `if(WIN32)` in CMake. They are NtfsDir2, NtfsAttr,
  NtfsDump, NtfsUndel and NtfsFuzzer.
- `include/`, `src/` and `NTFSLibTests/` MUST NOT contain a `friend` declaration. A class MUST get
  what it needs from another class through that class's methods, not through friendship.
  `.github/scripts/check-no-friend.py` enforces it in CI.
- Public headers in `include/ntfs-browser/` are the only consumer surface. They MUST NOT include
  spdlog, Crypto++, `<windows.h>`, or any feature macro. Win32 types come from the `win-types.h` shim.
  One header is the exception for spdlog: `include/ntfs-browser/log/spdlog-adapter.h`. It is opt-in
  and header-only, the library never includes it, and no other public header MAY include spdlog.
- Core classes that depend on cache behaviour MUST be templated on `Cache::Strategy`.
  Callers under `NoCache` MUST NOT assume attribute data outlives the next read.
- A template whose only parameter is a `Cache::Strategy` MUST NOT define a function in a header
  under `include/` or `src/`. A header declares it. Its code MUST live in one `.cpp`, which ends with
  the explicit instantiations for `NoCache` and `FullCache`. A template with another parameter is exempt.
  `.github/scripts/check-strategy-templates.py` enforces it in CI.
- `NtfsVolume`, `FileRecord` and `MftTree` MUST keep every data member and private method behind
  a private `Impl`.
- A symbol that crosses a shared-library boundary MUST carry `NTFS_BROWSER_EXPORT`.
  An internal symbol that tests reach MUST carry `NTFS_BROWSER_EXPORT_TESTS_ONLY`.
- Public types and functions MUST live in `NTFS_BROWSER_NAMESPACE`, in a sub-namespace per domain.
  Code MUST NOT spell out the namespace name.
- The library and its logging are single-threaded. `Log::Configure()`, `Log::SetSinks()` and every log message
  MUST run on one thread.
- Every source file MUST be listed explicitly in its folder's `CMakeLists.txt`. Globbing MUST NOT be used.
- Every public function in `include/ntfs-browser/` MUST have a purpose comment.
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

**Version**: 1.4.0 | **Ratified**: 2026-10-17 | **Last Amended**: 2026-10-20
