# CLAUDE.md

This file guides Claude Code when working with code in this repository.

## Project overview

`ntfs-browser` is a C++20, Windows-only library. It parses NTFS volumes directly: raw disk and MFT structures, not filesystem APIs. It is a modernized, read-only rewrite of the old CodeProject "An NTFS Parser Lib" (BSD-3c).

## Build

Windows and MSVC are the primary target. MFT/BPB parsing assumes Win32. The MFC demo apps, the unit tests, and `NtfsFuzzer` are Windows-only. The build requires submodules (`3rdparty/gsl`, `3rdparty/Catch2`, `3rdparty/spdlog`, `3rdparty/cryptopp`, `3rdparty/cryptopp-cmake`, `3rdparty/frozen`). Clone with `--recurse-submodules`, or run `git submodule update --init --recursive`.

```
cmake -S . -B build
cmake --build build --config Debug
```

An existing configured `build/` directory (Visual Studio generator) is already present in this repo. You can also open `build/NtfsBrowser.slnx` in Visual Studio.

`BUILD_SHARED_LIBS` (default OFF) selects a static or shared `NtfsBrowser` lib. spdlog follows it, and is linked PRIVATE: it never appears in a public header. CI (`.github/workflows/cmake.yml`) builds both Debug and Release, and both static and shared, on `windows-latest`. Crypto++ (`cryptopp-cmake` over `3rdparty/cryptopp`) does not follow it: it is always static, and linked PRIVATE too. `-DNTFS_BROWSER_USE_INSTALLED_CRYPTOPP=ON` links an installed copy (eg. vcpkg) instead. On MSVC the root [CMakeLists.txt](CMakeLists.txt) works around two Crypto++ 8.9 problems: MASM object directories the Visual Studio generator does not create, and `stdext` iterators the newest MSVC STL dropped ([cmake/cryptopp-stdext-compat.h](cmake/cryptopp-stdext-compat.h)).

The `NtfsBrowser` library itself, and the `NtfsFuzzerAfl` target ([NTFSLibTests/fuzz/](NTFSLibTests/fuzz/)), also configure and build on Linux with plain GCC: `cmake -S . -B build-linux && cmake --build build-linux`, checked with GCC 15 under WSL. `include/ntfs-browser/win-types.h` shims the handful of Windows typedefs (`BYTE`, `DWORD`, `LARGE_INTEGER`, ...) that the on-disk struct layouts and the public API are expressed in. Real Win32 API usage — `Win32DiskReader`, and drive-letter/path-based `NtfsVolume`/`FileReader` construction — is `#ifdef _WIN32`-guarded out. Everything else — the MFC demo apps, the unit tests, and the clang-oriented `NtfsFuzzer` — stays Windows/MSVC-only. CMake skips them (`if(WIN32)`) on other platforms.

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

`NTFSLibTests/ntfsattr`, `ntfsdir`, `ntfsdump`, `ntfsundel` are older sample/demo apps. `ntfsdir` is the simplest: it opens a volume, parses the root `FileRecord`, walks down to a path, and traverses entries. `NTFSLibTests/ntfsmftlist` lists every `$MFT` record through `MftTree`.

## Logging

All library diagnostics go through spdlog (`3rdparty/spdlog`, pinned to v1.17.0), behind the levelled `LogTrace`/`LogDebug`/`LogInfo`/`LogWarn`/`LogError`/`LogException` entry points in [src/ntfs-common.h](src/ntfs-common.h). Every level is always compiled in; filtering is runtime-only. spdlog MUST NOT appear in `include/ntfs-browser/*.h`, nor in any `src/*.h` the unit tests include.

[include/ntfs-browser/log.h](include/ntfs-browser/log.h) is the public surface: a `Log::Level` enum, a `Log::Config`, a `noexcept Log::Configure()`, and `Log::ParseOption()` for one `--log` argument.

The console-app executables (`NtfsDir`, `NtfsDir2`, `NtfsMftList`, `NtfsFuzzer`, `NtfsFuzzerAfl`) accept the option; the MFC dialog apps have no command line and do not.

```
--log=<console|file>:<off|error|warn|info|debug|trace>[:<path>]
```

The option MAY be repeated, once per target. It splits on the first two colons only, so `C:\dir\ntfs.log` survives. The path field belongs to the `file` target; without it the file sink writes `ntfs-browser.log` in the current directory.

`Log::Config::file_path` is a `std::filesystem::path`, and the build defines `SPDLOG_WCHAR_FILENAMES`, so on Windows a log path stays wide all the way to `_wfsopen` and is not limited to the active ANSI code page. The four console executables therefore have a `wmain`; `NtfsFuzzerAfl`, which also builds on Linux, picks `wmain`/`main` and the matching `argv` character type through `NTFS_FUZZ_MAIN`/`ArgChar`. `ParseOption()` has a `std::wstring_view` overload on Windows for that wide `argv`, alongside the portable `std::string_view` one.

The console target is split by severity: `error` and `warn` go to stderr, `info`, `debug` and `trace` go to stdout. A message reaches exactly one stream. With no `Configure()` call the console target sits at `warn` and there is no file sink, so a consumer sees warnings and errors on stderr and nothing on stdout.

`NtfsFuzzerAfl` defaults to `--log=console:trace`, which is what the regression corpus in [NTFSLibTests/unit-tests/fuzzer-regression-tests.cpp](NTFSLibTests/unit-tests/fuzzer-regression-tests.cpp) asserts against.

Neither the library nor its logging is thread safe, and the sinks are spdlog's single-threaded `_st` variants, so a message takes no lock. `Configure()` and every emitted message MUST come from one thread. A thread-safe sink is the answer if that ever has to change.

The unit tests pin the library logger to a trace-level capturing spdlog sink before `main()` ([NTFSLibTests/unit-tests/test-log-sink.h](NTFSLibTests/unit-tests/test-log-sink.h)). A test that calls `Configure()` itself MUST call `InstallCaptureSink()` again afterwards, since `Configure()` replaces the logger's sinks wholesale.

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

## Architecture

### Strategy-templated core

Almost every core class is templated on `Strategy` ([include/ntfs-browser/strategy.h](include/ntfs-browser/strategy.h)): `Strategy::NO_CACHE` or `Strategy::FULL_CACHE`. This propagates through `NtfsVolume<S>` → `FileReader<S>` → `FileRecord<S>` → `AttrBase<S>` and all `Attr*<S>` subclasses. `FULL_CACHE` retains every read cluster in a map for reuse. `NO_CACHE` re-reads and returns views into a short-lived buffer instead. Attributes under `NO_CACHE` hold raw pointers or spans into that buffer. Callers MUST NOT assume attribute data outlives the next read. Pick the strategy that matches lifetime needs when writing code that touches these templates.

### Read path / object graph

- `IDiskReader` ([include/ntfs-browser/disk-reader.h](include/ntfs-browser/disk-reader.h)) abstracts "get raw bytes from a backing store" behind `Open()`/`ReadInto()`. `Win32DiskReader` is the production implementation: a real disk/device handle, or a plain file treated the same way via `CreateFileW`. Tests substitute `MemoryDiskReader` or `SequentialDiskReader`.
- `NtfsVolume<S>` ([include/ntfs-browser/ntfs-volume.h](include/ntfs-browser/ntfs-volume.h)) owns a `FileReader<S>`. It can be constructed from a drive letter, an arbitrary path (device or image file), or an already-open `IDiskReader` (for tests/fakes). It parses the boot sector (BPB) to learn sector, cluster, file-record, and index-block sizes. It then locates and parses the `$MFT` file record itself (`mft_record_`), to resolve further file records through possibly-fragmented `$MFT` data runs.
- `FileRecord<S>` ([include/ntfs-browser/file-record.h](include/ntfs-browser/file-record.h)) represents one MFT file record. `ParseFileRecord(fileRef)` reads it. `ParseAttrs()` walks and instantiates its attributes; an optional `Mask` can skip unwanted attribute types. Set the mask before parsing, to avoid wasted work. Directory traversal (`TraverseSubEntries`, `FindSubEntry`) walks `$INDEX_ROOT`/`$INDEX_ALLOCATION` B-tree index entries. This includes index entries that only reach a file via `$ATTRIBUTE_LIST`: a record's attributes can be split across an extension record.
- `AttrBase<S>` ([include/ntfs-browser/attr-base.h](include/ntfs-browser/attr-base.h)) is the base for all attribute wrappers (`AttrResident`, `AttrNonResident`, and the concrete `Attr*` types in `src/`, e.g. `attr-file-name`, `attr-index-root`, `attr-std-info`, `attr-list` for `$ATTRIBUTE_LIST`). Resident and non-resident attributes differ in how `GetData()`/`ReadData()` fetch bytes: inline in the record, versus following data runs through clusters.
- Attribute-level compression (`FILE_ATTRIBUTE_COMPRESSED`) is handled inside `AttrNonResident<S>`. A non-zero `comp_unit_size` in the attribute header makes it read each compression unit's real and sparse runs and LZNT1-decompress it through [src/lznt1/decompress.h](src/lznt1/decompress.h). `ReadData()`/`GetData()` therefore return plain bytes either way, and callers never see a compression unit. Decompression is read-only: nothing re-compresses.
- EFS encryption (`FILE_ATTRIBUTE_ENCRYPTED`) is decrypted inside `AttrNonResident<S>::ReadVirtualClustersRaw()`. After `ParseAttrs()`, `FileRecord<S>` reads the `$EFS` stream (a `$LOGGED_UTILITY_STREAM`, non-resident on a real volume) and gives every `$DATA` stream whose header flag has 0x4000 a shared `Efs::Context` ([src/efs/efs-context.h](src/efs/efs-context.h)). The context resolves the FEK once, on the first read, through an `Efs::IEfsKeyProvider` ([include/ntfs-browser/efs.h](include/ntfs-browser/efs.h)). A failure is cached too. Decryption runs on the copy in the caller's buffer, per 512-byte sector: AES, 3DES or DESX in CBC mode, with an IV derived from the sector's stream offset. It MUST NOT run on the span `ReadClusters()` returns, since under `FULL_CACHE` that is the shared ciphertext. Sparse runs are not decrypted. Crypto++ or BCrypt runs the cipher, chosen by `Efs::SetCipherBackend()`. Crypto++, spdlog and `<windows.h>` MUST NOT appear in a public header, nor in a `src/*.h` the unit tests include. The provider comes from `NtfsVolume::SetEfsKeyProvider()`. When none was set, the first decryption creates one over `CurrentUser\My`. `SetEfsKeyProvider(nullptr)` disables decryption. A test MUST call it, and MUST NOT reach the real certificate store. Only AES-256 is checked against real data. Read-only: nothing encrypts.
- `MftTree` ([include/ntfs-browser/mft-tree.h](include/ntfs-browser/mft-tree.h)) rebuilds the whole namespace without directory indexes. It reads every `$MFT` record once, and links each record under the parents its own `$FILE_NAME` attributes name. A parent reference packs a 48-bit record number and a 16-bit sequence number. It is valid when the sequences match, when it carries sequence 0, or when the parent was freed and its sequence is one more (NTFS bumps it on deletion). Record 5 is always the root, even when its own record is lost. Extension records are skipped: their attributes come through the base record's `$ATTRIBUTE_LIST`. The result is plain owned data, not tied to a strategy, but the scan SHOULD run on a `NO_CACHE` volume: `FULL_CACHE` would keep the whole `$MFT` cached. `NtfsMftList` ([NTFSLibTests/ntfsmftlist/main.cpp](NTFSLibTests/ntfsmftlist/main.cpp)) lists its output. `TraverseSubEntries(..., true)` is the lighter, per-directory recovery: it also scans the `$INDEX_ALLOCATION` blocks no index pointer reaches.

### Directory structure

- `include/ntfs-browser/` — public API headers. These are what consumers of the library include.
- `src/` — implementation, plus internal-only headers not exposed publicly: `src/data/` (on-disk struct layouts: boot sector BPB, file record header, index block/entry, run entry), `src/attr/` (attribute header/type layouts), `src/flag/` (bitflag enums for filename/index-entry/std-info attributes), `src/efs/` (EFS: `$EFS` parser, FEK, sector ciphers for both backends, Windows key providers). Tests include from `src/` directly (see `NTFSLibTests/unit-tests/CMakeLists.txt`), to reuse these on-disk struct layouts when building fake images, rather than duplicating byte offsets.
- `NTFSLibTests/` — Catch2 unit tests, plus the older MFC-based sample/demo apps.

### Callbacks

Two callback mechanisms exist. `AttrRawCallback` (installed via `InstallAttrRawCB` on `NtfsVolume`/`FileRecord`) fires during raw attribute parsing; it can veto or discard an attribute before it's wrapped. `ATTRS_CALLBACK`/`SUBENTRY_CALLBACK` are post-parse traversal callbacks (`TraverseAttrs`, `TraverseSubEntries`) for consumer code, matching the style used in the sample apps.

### Fuzzing

Both fuzzers drive the same path — open a `NO_CACHE` volume, parse the root `FileRecord`, `TraverseSubEntries` — and treat thrown C++ exceptions as "handled" (bad input rejected), leaving only genuine crashes (access violations, CRT aborts) to escape as real findings:

- `NtfsFuzzer` ([NTFSLibTests/fuzz/main.cpp](NTFSLibTests/fuzz/main.cpp), Windows-only) feeds an endless RNG-backed byte stream through `SequentialDiskReader`; a SEH handler around each iteration catches crashes and prints the seed to repro with `NtfsFuzzer --seed <seed>`.
- `NtfsFuzzerAfl` ([NTFSLibTests/fuzz/afl-main.cpp](NTFSLibTests/fuzz/afl-main.cpp)) is a classic afl-gcc/afl-g++ file-input harness (`NtfsFuzzerAfl <path>`, or `afl-fuzz -i in -o out -- ./NtfsFuzzerAfl @@`); it loops the input via `LoopingDiskReader` instead of generating fresh bytes, and builds on Linux too (portable C++, no SEH). It runs each input once per cache strategy. `--inject-read-failures` adds one run per strategy for each of the first 16 `ReadInto()` calls, each failing that one read. It reaches the disk-read error paths, but multiplies the run time by 17. AFL runs MUST NOT pass it.
- Crashing inputs found this way get saved into [NTFSLibTests/fuzz/data/](NTFSLibTests/fuzz/data/) as a regression corpus, replayed by the Catch2 test in [NTFSLibTests/unit-tests/fuzzer-regression-tests.cpp](NTFSLibTests/unit-tests/fuzzer-regression-tests.cpp), which runs the actual `NtfsFuzzerAfl` binary with `--inject-read-failures` against each saved file and checks for a clean exit (plus, for entries in its `kExpectedErrorMessages` table, the specific parse-error message the fix is supposed to produce).
