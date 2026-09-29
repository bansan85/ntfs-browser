# CLAUDE.md

This file guides Claude Code when working with code in this repository.

## Project overview

`ntfs-browser` is a C++20 library, Windows-first. It parses NTFS volumes directly: raw disk and MFT structures, not filesystem APIs. It is a modernized, read-only rewrite of the old CodeProject "An NTFS Parser Lib" (BSD-3c).

## Build

Windows and MSVC are the primary target. The MFC demo apps and `NtfsFuzzer` are Windows-only; the Catch2 unit tests build and run on Linux too (see the GCC/Linux paragraph below). The build requires submodules (`3rdparty/gsl`, `3rdparty/Catch2`, `3rdparty/spdlog`, `3rdparty/cryptopp`, `3rdparty/cryptopp-cmake`, `3rdparty/frozen`). Clone with `--recurse-submodules`, or run `git submodule update --init --recursive`.

```
cmake -S . -B build
cmake --build build --config Debug --parallel
```

An existing configured `build/` directory (Visual Studio generator) is already present in this repo. You can also open `build/NtfsBrowser.slnx` in Visual Studio.

[CMakePresets.json](CMakePresets.json) defines the presets: `static`, `shared`, `static-vcpkg`, `shared-vcpkg`, and the reduced-feature ones. None names a generator, so each takes the platform default: Visual Studio on Windows (multi-config, build preset defaults to Debug, no `compile_commands.json`), and the CMake default on Linux (pass `-G Ninja`). Each builds into `build/<preset>/`. Every configure preset also has a workflow preset (`cmake --workflow --preset <name>`). VS Code uses them (`cmake.useCMakePresets`). The `*-vcpkg` presets hardcode `CMAKE_TOOLCHAIN_FILE` to `H:/repos/vcpkg`.

```
cmake --preset static
cmake --build --preset static --parallel
```

`BUILD_SHARED_LIBS` (default OFF) selects a static or shared `NtfsBrowser` lib. spdlog follows it, and is linked PRIVATE: it never appears in a public header. Crypto++ (`cryptopp-cmake` over `3rdparty/cryptopp`) does not follow it: it is always static, and linked PRIVATE too. On MSVC the root [CMakeLists.txt](CMakeLists.txt) works around two Crypto++ 8.9 problems: MASM object directories the Visual Studio generator does not create, and `stdext` iterators the newest MSVC STL dropped ([cmake/cryptopp-stdext-compat.h](cmake/cryptopp-stdext-compat.h)).

`NTFS_BROWSER_USE_INSTALLED_{GSL,FROZEN,CATCH2,SPDLOG,CRYPTOPP}` each link an installed copy instead of the submodule. All five default ON under the vcpkg toolchain ([vcpkg.json](vcpkg.json)), OFF otherwise. `NTFS_BROWSER_ENABLE_TESTING` defaults ON only in a top-level build, not under FetchContent.

Three feature options, all ON by default, compile code in or out: `NTFS_BROWSER_ENABLE_DECOMPRESSION` (LZNT1), `NTFS_BROWSER_ENABLE_EFS_CRYPTOPP` and `NTFS_BROWSER_ENABLE_EFS_BCRYPT` (the two EFS cipher backends). Turning one off drops its sources, its link dependency and its call sites. Each reaches C++ as a same-named macro, PRIVATE to the library and mirrored on `NtfsBrowserTests`. A `.cpp` or a `src/*.h` tests it with `#if`. A public header MUST NOT: consumers never see these macros. The BCrypt macro is defined on Linux too, so code MUST pair it with `_WIN32`. `NTFS_BROWSER_EFS_MASTER` (CMake) means at least one EFS backend is compiled in. It gates the backend-agnostic EFS sources and the EFS tests. C++ spells the same condition `defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))`.

Source lists are explicit, not globbed. A new file MUST be added to [src/CMakeLists.txt](src/CMakeLists.txt) or [NTFSLibTests/unit-tests/CMakeLists.txt](NTFSLibTests/unit-tests/CMakeLists.txt), inside the matching feature gate if it has one.

CI ([.github/workflows/cmake.yml](.github/workflows/cmake.yml)) builds Debug and Release, static and shared, on `windows-latest`. The Windows legs use the presets' default Visual Studio generator and name the configuration on build and `ctest`. Linux legs pass `-G Ninja`. It also builds each reduced configuration once: one feature option off, or both EFS backends off. Every leg runs `ctest`. A `test-data` job first fetches the corpus images on Ubuntu into a cache, which each Windows leg restores. A second job builds [fetchcontent-consumer/](fetchcontent-consumer/): a separate project that pulls this repo in through FetchContent, shared, with every feature ON.

The `NtfsBrowser` library itself, the `NtfsFuzzerAfl` target ([NTFSLibTests/fuzz/](NTFSLibTests/fuzz/)), and the Catch2 unit tests ([NTFSLibTests/unit-tests/](NTFSLibTests/unit-tests/)) also configure and build on Linux with plain GCC: `cmake -S . -B build-linux && cmake --build build-linux --parallel`, checked with GCC 15 under WSL. `include/ntfs-browser/win-types.h` shims the handful of Windows typedefs (`BYTE`, `DWORD`, `LARGE_INTEGER`, ...) that the on-disk struct layouts and the public API are expressed in; unit-test-only helpers (`fake-ntfs-image.h`, the fake `IDiskReader`s) use the same shim instead of `<windows.h>` directly. Real Win32 API usage — `Win32DiskReader`, drive-letter/path-based `NtfsVolume`/`FileReader` construction, and the child-process spawning `logging-tests.cpp`/`fuzzer-regression-tests.cpp` need to drive `NtfsFuzzerAfl` — is `#ifdef _WIN32`-guarded out. [NTFSLibTests/unit-tests/child-process.h](NTFSLibTests/unit-tests/child-process.h) is that spawning code's cross-platform seam: `CreateProcess`/pipes on Windows, `posix_spawn` on other platforms, behind the same two functions either way. A handful of individual `TEST_CASE`s stay `_WIN32`-guarded where the behaviour itself is Windows-only (the wide `Log::ParseOption()` overload, the ANSI-code-page test). Everything else — the MFC demo apps and the clang-oriented `NtfsFuzzer` — stays Windows/MSVC-only. CMake skips them (`if(WIN32)`) on other platforms.

## Tests

Tests use Catch2 (vendored under `3rdparty/Catch2`). CTest registers them via `catch_discover_tests`.

```
cmake --build build --config Debug --target NtfsBrowserTests --parallel
ctest --test-dir build -C Debug
```

`ctest -R <regex>` runs the tests whose name matches. A Catch2 tag or wildcard needs the binary itself (`build/<preset>/NTFSLibTests/unit-tests/NtfsBrowserTests.exe` in a preset tree). There, a `[` inside a test name starts a tag: escape it as `\[`.
```
build/NTFSLibTests/unit-tests/Debug/NtfsBrowserTests.exe "<test name>"
build/NTFSLibTests/unit-tests/Debug/NtfsBrowserTests.exe "[efs]"
build/NTFSLibTests/unit-tests/Debug/NtfsBrowserTests.exe "LZNT1*"
```

Test sources live in [NTFSLibTests/unit-tests/](NTFSLibTests/unit-tests/). Tests must not touch a real disk. They build synthetic NTFS images in memory ([fake-ntfs-image.h](NTFSLibTests/unit-tests/fake-ntfs-image.h)), using the library's own on-disk struct layouts from `src/data` and `src/attr`. They serve those images through fake `IDiskReader` implementations: [memory-disk-reader.h](NTFSLibTests/unit-tests/memory-disk-reader.h) (random-access, whole buffer in memory) or [sequential-disk-reader.h](NTFSLibTests/unit-tests/sequential-disk-reader.h) (offset-ignoring, chunk-at-a-time). Prefer extending these fakes over adding new test scaffolding.

The corpus tests are the exception to synthetic images: `dftt-ntfs-*-tests.cpp`, `nps-ntfs1-*tests.cpp` and `ntfs-samples-tests.cpp`. They open forensic image files outside the repo, never a device. [.github/scripts/fetch-test-data.py](.github/scripts/fetch-test-data.py) fetches them into `test-data/`. The `NTFS_BROWSER_TEST_*` CMake cache variables, or same-named environment variables, locate them, through [corpus-test-support.h](NTFSLibTests/unit-tests/corpus-test-support.h); the README lists them. A test MUST reach its image through `RequireCorpusImage()`: it `SKIP`s when the image is absent, or `FAIL`s under `NTFS_BROWSER_REQUIRE_TEST_DATA`, which CI sets. `ntfs.raw` (64 GiB) and `ntfs_extremely_fragmented_mft.raw` (256 GiB) are never fetched: their tests use a plain `SKIP`. [partition-disk-reader.h](NTFSLibTests/unit-tests/partition-disk-reader.h) serves a whole-disk image, offsetting every read into its NTFS partition.

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

CI enforces `clang-format` (config in [.clang-format](.clang-format)) and `gersemi` (config in [.gersemirc](.gersemirc)) for `CMakeLists.txt`. See [.github/workflows/format.yml](.github/workflows/format.yml): a push to `main`, or a PR into it, fails if formatting changes anything. [pyproject.toml](pyproject.toml) pins `clang-format==23.1.0`; another version can format differently. [uv](https://docs.astral.sh/uv/) resolves both tools from [uv.lock](uv.lock) into the gitignored `.venv/`. Run before committing:
```
uv run bash ./.github/scripts/format.sh
```
`.clang-tidy` enables nearly all checks (`Checks: '*'`, minus a short exclusion list), with `WarningsAsErrors: '*'`. CI ([.github/workflows/clang-tidy.yml](.github/workflows/clang-tidy.yml)) runs it on Ubuntu, over the Linux configuration's `compile_commands.json`. `#ifdef _WIN32` code and the Windows-only targets are therefore never linted in CI. A local run needs a `compile_commands.json`, which the Visual Studio generator does not write: configure a preset with `-G Ninja` from an MSVC developer environment, then `clang-tidy -p build/static <file>`.

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

### Templates and symbol export

Class templates are declared in headers and defined in `src/*.cpp`. Each `.cpp` ends with explicit instantiations for both strategies (`template class FileRecord<Strategy::NO_CACHE>;`, then `FULL_CACHE`). A new class template, or a new member function template, MUST be instantiated there too, or it will not link. Most concrete attribute wrappers also take their base class as a parameter: `AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>`. `AttrList` and `AttrBitmap` exist over both a resident and a non-resident base.

A shared build exports symbol by symbol. `generate_export_header` writes `NTFS_BROWSER_EXPORT` into `src/include/ntfs-browser/export.h` in the build tree. A public class or function that crosses the DLL boundary carries it. An internal symbol the unit tests reach through a `src/` header carries `NTFS_BROWSER_EXPORT_TESTS_ONLY` ([src/internal-export.h](src/internal-export.h)) instead. It exports only under `NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS`, which the unit tests' CMakeLists defines on the library. The class-level macro does not reach a member function template: each explicit instantiation of one repeats it. A test that calls a new internal symbol links in a static build, but fails in a shared one until that symbol is marked.

### Read path / object graph

- `IDiskReader` ([include/ntfs-browser/disk-reader.h](include/ntfs-browser/disk-reader.h)) abstracts "get raw bytes from a backing store" behind `Open()`/`ReadInto()`. `Win32DiskReader` is the production implementation: a real disk/device handle, or a plain file treated the same way via `CreateFileW`. Tests substitute `MemoryDiskReader` or `SequentialDiskReader`.
- `NtfsVolume<S>` ([include/ntfs-browser/ntfs-volume.h](include/ntfs-browser/ntfs-volume.h)) owns a `FileReader<S>`. It can be constructed from a drive letter, an arbitrary path (device or image file), or an already-open `IDiskReader` (for tests/fakes). It parses the boot sector (BPB) to learn sector, cluster, file-record, and index-block sizes. It then locates and parses the `$MFT` file record itself (`mft_record_`), to resolve further file records through possibly-fragmented `$MFT` data runs. Every constructor takes an optional `VolumeOptions` ([include/ntfs-browser/volume-options.h](include/ntfs-browser/volume-options.h)), fixed for the volume's lifetime and read back through `GetOptions()`. Both flags default off. `include_deleted` gates content, not just visibility: `FileRecord::ParseFileRecord()` still reads a freed record's header (`IsDeleted()` works), but `ParseAttrs()` returns false on it unless this is on. `recover_errors` chooses, for a damaged file record, index block, or `$ATTRIBUTE_LIST`, between rejecting it whole (off, the default) and salvaging what parses (on, today's long-standing behaviour). Rejection is one item: the enclosing scan or walk moves on to the next one. A salvageable condition logs Warn when strict and Info when recovering, through `LogRecoverable()` ([src/ntfs-common.h](src/ntfs-common.h)). The volume's own metadata reads ($Volume, `mft_record_`, and the extension records `ResolveMftDataExtents()` opens) bypass the `include_deleted` gate, so a freed `$MFT` or `$Volume` record doesn't take the whole volume down.
- `FileRecord<S>` ([include/ntfs-browser/file-record.h](include/ntfs-browser/file-record.h)) represents one MFT file record. `ParseFileRecord(fileRef)` reads it. `ParseAttrs()` walks and instantiates its attributes; an optional `Mask` can skip unwanted attribute types. Set the mask before parsing, to avoid wasted work. Directory traversal (`TraverseSubEntries`, `FindSubEntry`) walks `$INDEX_ROOT`/`$INDEX_ALLOCATION` B-tree index entries. This includes index entries that only reach a file via `$ATTRIBUTE_LIST`: a record's attributes can be split across an extension record.
- `AttrBase<S>` ([include/ntfs-browser/attr-base.h](include/ntfs-browser/attr-base.h)) is the base for all attribute wrappers (`AttrResident`, `AttrNonResident`, and the concrete `Attr*` types in `src/`, e.g. `attr-file-name`, `attr-index-root`, `attr-std-info`, `attr-list` for `$ATTRIBUTE_LIST`). Resident and non-resident attributes differ in how `GetData()`/`ReadData()` fetch bytes: inline in the record, versus following data runs through clusters.
- Attribute-level compression (`FILE_ATTRIBUTE_COMPRESSED`) is handled inside `AttrNonResident<S>`. A non-zero `comp_unit_size` in the attribute header makes it read each compression unit's real and sparse runs and LZNT1-decompress it through [src/lznt1/decompress.h](src/lznt1/decompress.h). `ReadData()`/`GetData()` therefore return plain bytes either way, and callers never see a compression unit. Decompression is read-only: nothing re-compresses.
- EFS encryption (`FILE_ATTRIBUTE_ENCRYPTED`) is decrypted inside `AttrNonResident<S>::ReadVirtualClustersRaw()`. After `ParseAttrs()`, `FileRecord<S>` reads the `$EFS` stream (a `$LOGGED_UTILITY_STREAM`, non-resident on a real volume) and gives every `$DATA` stream whose header flag has 0x4000 a shared `Efs::Context` ([src/efs/efs-context.h](src/efs/efs-context.h)). The context resolves the FEK once, on the first read, through an `Efs::IEfsKeyProvider` ([include/ntfs-browser/efs.h](include/ntfs-browser/efs.h)). A failure is cached too. Decryption runs on the copy in the caller's buffer, per 512-byte sector: AES, 3DES or DESX in CBC mode, with an IV derived from the sector's stream offset. It MUST NOT run on the span `ReadClusters()` returns, since under `FULL_CACHE` that is the shared ciphertext. Sparse runs are not decrypted. Crypto++ or BCrypt runs the cipher, chosen by `Efs::SetCipherBackend()`. Crypto++, spdlog and `<windows.h>` MUST NOT appear in a public header, nor in a `src/*.h` the unit tests include. The provider comes from `NtfsVolume::SetEfsKeyProvider()`. When none was set, the first decryption creates one over `CurrentUser\My`. `SetEfsKeyProvider(nullptr)` disables decryption. A test MUST call it, and MUST NOT reach the real certificate store. Only AES-256 is checked against real data. Read-only: nothing encrypts.
- `MftTree` ([include/ntfs-browser/mft-tree.h](include/ntfs-browser/mft-tree.h)) rebuilds the whole namespace without directory indexes. It reads every `$MFT` record once, and links each record under the parents its own `$FILE_NAME` attributes name. A parent reference packs a 48-bit record number and a 16-bit sequence number. It is valid when the sequences match, when it carries sequence 0, or when the parent was freed and its sequence is one more (NTFS bumps it on deletion). Record 5 is always the root, even when its own record is lost. Extension records are skipped: their attributes come through the base record's `$ATTRIBUTE_LIST`. The result is plain owned data, not tied to a strategy, but the scan SHOULD run on a `NO_CACHE` volume: `FULL_CACHE` would keep the whole `$MFT` cached. `NtfsMftList` ([NTFSLibTests/ntfsmftlist/main.cpp](NTFSLibTests/ntfsmftlist/main.cpp), `--deleted`/`--recover`) lists its output; a damaged record is dropped when strict, kept with partial data when recovering, either way counted in `Stats().damaged`. `FileRecord::TraverseSubEntries()` is the lighter, per-directory recovery: with the volume's `recover_errors` on, it also scans the `$INDEX_ALLOCATION` blocks no index pointer reaches, capped at `kMaxOrphanScanBlocks`; an entry found this way is reported only if its parent reference matches, and, with `include_deleted` off, only if the record it names is still in use under a matching sequence number.

### Directory structure

- `include/ntfs-browser/` — public API headers. These are what consumers of the library include.
- `src/` — implementation, plus internal-only headers not exposed publicly: `src/data/` (on-disk struct layouts: boot sector BPB, file record header, index block/entry, run entry), `src/attr/` (attribute header/type layouts), `src/flag/` (bitflag enums for filename/index-entry/std-info attributes), `src/efs/` (EFS: `$EFS` parser, FEK, sector ciphers for both backends, Windows key providers). Tests include from `src/` directly (see `NTFSLibTests/unit-tests/CMakeLists.txt`), to reuse these on-disk struct layouts when building fake images, rather than duplicating byte offsets.
- `NTFSLibTests/` — Catch2 unit tests, plus the older MFC-based sample/demo apps.
- `docs/[MS-XCA].pdf` — Microsoft's compression spec, LZNT1 included. The LZNT1 tests cite its sections.

### Callbacks

Two callback mechanisms exist. `AttrRawCallback` (installed via `InstallAttrRawCB` on `NtfsVolume`/`FileRecord`) fires during raw attribute parsing; it can veto or discard an attribute before it's wrapped. `ATTRS_CALLBACK`/`SUBENTRY_CALLBACK` are post-parse traversal callbacks (`TraverseAttrs`, `TraverseSubEntries`) for consumer code, matching the style used in the sample apps.

### Fuzzing

Both fuzzers drive the same path — open a `NO_CACHE` volume, parse the root `FileRecord`, `TraverseSubEntries` — and treat thrown C++ exceptions as "handled" (bad input rejected), leaving only genuine crashes (access violations, CRT aborts) to escape as real findings:

- `NtfsFuzzer` ([NTFSLibTests/fuzz/main.cpp](NTFSLibTests/fuzz/main.cpp), Windows-only) feeds an endless RNG-backed byte stream through `SequentialDiskReader`; a SEH handler around each iteration catches crashes and prints the seed to repro with `NtfsFuzzer --seed <seed>`.
- `NtfsFuzzerAfl` ([NTFSLibTests/fuzz/afl-main.cpp](NTFSLibTests/fuzz/afl-main.cpp)) is a classic afl-gcc/afl-g++ file-input harness (`NtfsFuzzerAfl <path>`, or `afl-fuzz -i in -o out -- ./NtfsFuzzerAfl @@`); it loops the input via `LoopingDiskReader` instead of generating fresh bytes, and builds on Linux too (portable C++, no SEH). It runs each input once per cache strategy, and once per strategy for each `VolumeOptions` mode (strict and fully recovering). `--inject-read-failures` adds one run per strategy per mode for each of the first 16 `ReadInto()` calls, each failing that one read. It reaches the disk-read error paths, but multiplies the run time by roughly 34 (2 modes x (1 base + 16 injected)) per strategy. AFL runs MUST NOT pass it.
- Crashing inputs found this way get saved into [NTFSLibTests/fuzz/data/](NTFSLibTests/fuzz/data/) as a regression corpus, replayed by the Catch2 test in [NTFSLibTests/unit-tests/fuzzer-regression-tests.cpp](NTFSLibTests/unit-tests/fuzzer-regression-tests.cpp), which runs the actual `NtfsFuzzerAfl` binary with `--inject-read-failures` against each saved file and checks for a clean exit (plus, for entries in its `kExpectedErrorMessages` table, the specific parse-error message the fix is supposed to produce).
