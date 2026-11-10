# CLAUDE.md

`ntfs-browser` is a C++20, Windows-first, read-only NTFS parser library. It reads raw disk and MFT structures, not filesystem APIs. It is a rewrite of the CodeProject "An NTFS Parser Lib" (BSD-3c).

## Build and test

Submodules are required: `git submodule update --init --recursive`.

```
cmake --preset static
cmake --build --preset static --parallel
ctest --test-dir build/static -C Debug
```

- Presets (`static`, `shared`, `*-vcpkg`, reduced-feature ones) use the Visual Studio 18 2026 generator. On Linux pass `-G Ninja`. Plain `cmake -S . -B build-linux` also works with GCC.
- One test: `ctest -R <regex>`, or the `NtfsBrowserTests` binary with a Catch2 name or tag (`"[efs]"`). A `[` in a test name starts a tag: escape it as `\[`.
- Feature options `NTFS_BROWSER_ENABLE_DECOMPRESSION`, `..._EFS_CRYPTOPP`, `..._EFS_BCRYPT`, `..._REGEX` compile code in or out. `REGEX` pulls PCRE2 (16 bit, JIT, static even in a shared build), and only `src/search/regex-matcher.cpp` includes it. Test the macro with `#if` in a `.cpp` or `src/*.h`. A public header MUST NOT test it. Pair the BCrypt macro with `_WIN32`.
- `NtfsFuzzer` is Windows-only. The library, unit tests, the console tools (`NtfsAttr`, `NtfsDump`, `NtfsUndel`, `NtfsDir`, `NtfsScanBench`, `NtfsCompare`) and `NtfsFuzzerAfl` also build on Linux. There is no MFC in the build.
- The build generates the fuzz inputs and the corpus traces into `build/<preset>/fuzz-generated/` (target `NtfsFuzzGenData`). `NtfsBrowserTests` depends on it. No such file is committed. See Fuzzing.
- `NtfsScanBench` (`NTFSLibTests/examples/ntfsscanbench/`) lists a volume with `--engine=mftlist` (the `MftTree` scan), `nocache` or `fullcache` (the directory walk of `tools/dir-walk`, also the oracle of the read ahead tests), or `all` three with a table. It takes the benchmark options. `NtfsBench` stays the replay, simulate and disk benchmark tool.
- The tools and tests read and write JSON through `NTFSLibTests/tools/device-access/json.h`, a thin layer over nlohmann/json (`3rdparty/nlohmann_json`, only `NtfsDeviceAccess` links it, in `json.cpp`). The library and every public header MUST NOT include it.
- Source lists are explicit, never globbed. A new file MUST be added to `src/CMakeLists.txt` or to the `CMakeLists.txt` of its test or tool folder, inside the matching feature gate.

## Formatting

Run before committing: `uv run bash ./.github/scripts/format.sh` (clang-format 23.1.0 and gersemi). CI fails on any change. `.clang-tidy` is strict (`WarningsAsErrors: '*'`) and runs on demand only.

## Commits

Conventional Commits, no scope (`fix:`, `feat:`, `refactor:`, `test:`, `docs:`, `chore:`, `build:`, `ci:`, `style:`, `perf:`). English, lowercase title, no trailing period, 50 characters at most, prefix included. Body wraps at 72. End with a `Co-Authored-By:` trailer naming the model.

## Writing style

- Short sentences, in comments and in docs. Use RFC 2119 keywords for obligations.
- Plain English: the vocabulary and grammar of an average 16-year-old student, with the technical depth of a senior engineer.
- Never use the em dash, in any language.
- State the idea. Never announce it first ("The key point is", "Here is why").
- No pseudo-cleft sentences ("What matters is...", "It is X that...").
- No emphatic presentational phrases, and no detached clause that fakes a subordinate ("..., which is why.").
- No epanorthosis (correcting a phrase right after saying it), no verbal anaphora (sentences that open with the same words), no binary antithesis ("not X but Y").
- No counted announcements ("There are three reasons") and no announced lists ("The following points:").
- A comment MUST say what the code cannot. It MUST NOT narrate the code.
- Every public function declared in a header (`include/ntfs-browser/*.h` or `src/**/*.h`) gets a purpose comment. So does a private method defined in a `.cpp`.
- Every constant gets a comment: what it is for, and why that value.
- A `TEST_CASE` or `SECTION` MUST NOT get a comment. Its name is the description.
- A boolean condition with four or more distinct elements across more than one kind of check gets one comment line saying what it means. Skip it if a log call in the same branch already says so.

## Architecture rules

- **Namespaces.** Write `NTFS_BROWSER_NAMESPACE::`, never the literal name. The root holds the public API (`Scan`, `Reader` and their types, from the five public headers, and the internal code of `src/search/`) and `IDiskReader`, `NtfsVolume`, `IndexEntry`, `Filename` and `VolumeOptions`. Everything else goes in a domain sub-namespace (`Attr`, `Data`, `Io`, `Mft`, `Cache`, `Alloc`, `Log`, `Efs`, ...). There is no `Search` namespace. A name in `src/search/` MUST NOT clash with a root or domain name. A file `<prefix>-*` lives in folder `<prefix>/`. Namespaces follow the domain, not the C++ entity kind.
- **Strategy templates.** Core classes are templated on `Cache::Strategy` (`NoCache`, `FullCache`). A template whose only parameter is the strategy MUST NOT define any function in a header: declare it, define it in the `.cpp`, and end the `.cpp` with explicit instantiations for both strategies. `.github/scripts/check-strategy-templates.py` enforces it. Under `NoCache`, attribute data is a view into a short-lived buffer. Callers MUST NOT assume it outlives the next read. Under `FullCache`, keep the `Cache::View`, not a bare span.
- **Shared builds export symbol by symbol.** Only a symbol of the five public headers carries `NTFS_BROWSER_EXPORT`: `Scan`, `Reader`, `RegexAvailable()`, `SetLogSink()`, `ToUtf8()` and `EntryFlags()`. Every internal symbol that tests, tools or fuzzers reach through `src/` carries `NTFS_BROWSER_EXPORT_TESTS_ONLY` (`src/internal-export.h`). It exports only when `NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS` is defined, which the library and `NtfsBrowserInternals` do in a build with tests. A build without tests exports the public symbols only. A member function template repeats the macro on each explicit instantiation.
- **Public surface and private root.** `include/ntfs-browser/` holds the public headers only: `scan.h`, `types.h`, `reader.h`, `reader-types.h`, `log.h`, plus `namespace.h` (generated) and `export.h` (generated). Every other header is internal and lives in `src/`, in the folder of its domain (`src/io/file-record.h`, `src/ntfs-volume.h`, `src/log/log.h`). Library sources, tests, tools, examples and fuzzers include an internal header in the `src/` relative form with quotes: `#include "io/file-record.h"`. The library target keeps `src/` private. A test, tool or fuzzer links the CMake interface target `NtfsBrowserInternals` (`NTFSLibTests/CMakeLists.txt`), which adds `src/` and the tests-only export macro. `NtfsPublicApiCheck` (`NTFSLibTests/public-api/`, ctest `PublicApiCheck`) links the library alone and includes the public headers only, so a public header that needs an internal one fails the build. Tuning, `Bench`, `win-types.h` (`FILETIME`), `IDiskReader`, `NtfsVolume`, `Log::Configure()` and the sinks are internal.
- **One class, one file pair.** Every `.cpp` in `src/` has a `.h` of the same name. The header lives in `include/ntfs-browser/` when it is public, and next to its `.cpp` when it is private. A class gets its own header and its own `.cpp`. A `.cpp` MUST NOT be split in two unless the class is split in two: each new `.cpp` holds a new class, with its own header.
- **Scan is the public entry point.** `Scan` (`include/ntfs-browser/scan.h`, types in `types.h`) lists a folder or a whole volume. It opens the volume itself from a target string (drive, image, device, plus an optional inside path). Its two headers MUST be self-sufficient: they include only the standard library, `namespace.h`, `export.h` and, for `scan.h`, `types.h`, and use no internal type, no `<windows.h>`, no spdlog, no PCRE2 and no feature macro. Dates are `Date` (100 ns ticks since 1601), never `FILETIME`. Pimpl: `Scan::Impl` is in `src/search/scan.cpp`. Engines, target parsing, matchers, the filter and the entry buffer live in `src/search/`. `Run(interval)` is `Poll()` in a loop on the caller's thread (woken by a condition variable of `ScanExchange`); only `Cancel()` MAY be called from another thread.
  - **Engines.** `MftList`, `DirectoryNoCache`, `DirectoryFullCache`. `Auto` takes `MftList` for the root of a volume and `DirectoryFullCache` for a folder, unless the filter needs a field that only one kind gives. An engine MUST give every entry to the `MatchingSink` it made, and MUST NOT test or count an entry itself. A pattern or filter on a field that the engine does not give ends the scan as `Failed`, never as an empty result.
  - **Pattern and filter.** `Options::pattern` (exact, wildcard, regex; name or full path) and `Options::filter` (a test on every field) are applied by the worker before an entry takes memory. Tests are joined by AND only. An unset field is not tested. `$UpCase` stays private: wildcard and caseless work runs in `src/search/`, and no public type holds a table. Regex needs `NTFS_BROWSER_ENABLE_REGEX` (`RegexAvailable()` tells); without it a regex scan ends as `Failed`.
  - **Threads and log.** `Start()` makes one worker thread. `Poll()`, the callback, `Cancel()` and the destructor stay on the creating thread. A batch is a view into a worker buffer, valid during the callback only. The worker captures its log (`Log::Detail::WorkerLog`) and `Poll()` replays it to the sinks. An engine MUST NOT touch the log sinks. The tests are in `NTFSLibTests/unit-tests/search/` and tag `[search]`: they replace the engine with `SetEngineFactoryForTests()` when they need a controlled worker.
- **Reader reads what Scan found.** `Reader` (`include/ntfs-browser/reader.h`, types in `reader-types.h`) takes the `Handle` that `Entry::GetHandle()` gives and reads the file behind it: `$STANDARD_INFORMATION`, every `$FILE_NAME`, the `$DATA` streams (list, `Read()` at an offset, `CopyTo()` a sink), the raw bytes of any attribute, and the data state of a deleted file. A handle is plain copyable values: record, sequence, name index, parent, deleted flag, volume serial. The Reader opens the volume once from the target (`NoCache`: a copy of a file stays small), checks the record against the handle on every call, and fails with a `ReadError`, never with wrong data, when the record is another file's, gone or damaged. Its two headers hold the same limits as Scan's: the standard library, `namespace.h`, `export.h` and `types.h` only. `log.h` follows the same limits. It is synchronous and MUST NOT run while a Scan runs. `Reader::Impl` is in `src/search/reader.cpp`. Tests are in `NTFSLibTests/unit-tests/search/` and tag `[search][reader]`. EFS tests set the key through `SetReaderKeyProviderForTests()` (`src/search/reader-probe.h`) and never reach the real certificate store. `NtfsUndel` is built on Scan and Reader and includes no `src/` header.
- **Pimpl.** `NtfsVolume<S>`, `FileRecord<S>` and `MftTree` keep all data members and private methods in a private nested `Impl`. A public header (and any `src/` header that a consumer of the library could see) MUST NOT declare them. `Impl` is defined in a `src/` header (`ntfs-volume-impl.h`, `io/file-record-impl.h`) or in the `.cpp` (`mft/tree.cpp`). Another class reaches that state through a method of the owner.
- **No `friend`** anywhere in `include/`, `src/` or `NTFSLibTests/`. Make a narrow public method instead. `check-no-friend.py` enforces it.
- **Logging.** Everything goes through `Log::Trace/Debug/Info/Warn/Error` in `src/log/ntfs-common.h`. spdlog MUST NOT appear in `include/ntfs-browser/*.h` or in any `src/*.h` the tests include, except the opt-in `src/log/spdlog-adapter.h`. A user of the library receives or silences the log through `SetLogSink()` (`include/ntfs-browser/log.h`): one `std::function<void(LogLevel, std::string_view)>` and a threshold, no spdlog. It sits on `Log::SetSinks()`. `Log::Configure()`, `ParseOption()` and the sinks stay internal, for the tools and the tests. Neither the library nor its logging is thread safe: `Configure()`, `SetSinks()` and every emitted message come from one thread. A worker thread captures its log and the owning thread replays it.
- **Crypto++, spdlog, PCRE2 and `<windows.h>`** MUST NOT appear in a public header. `src/win-types.h` includes `<windows.h>` on Windows because internal structs hold `FILETIME` by value. `Date` is the one public date.
- **Paths.** Code that opens a user-given path MUST go through `Io::ToExtendedLengthPath()`. Tests that create or list paths over 260 units MUST extend them too.
- **Names** are raw UTF-16 units. Collate through `$UpCase`, never the locale. A map or set keyed by a name MUST NOT fold case. Print names as UTF-8 (WTF-8 for unpaired surrogates) with `Utf::WideToUtf8()`.
- **Strict vs recovering.** `VolumeOptions::recover_errors` chooses between rejecting a damaged item and salvaging it. `include_deleted` decides which healthy items show. Call `Log::Recoverable()` only for a condition that strict mode would reject.
- **Instrumentation and tuning** are opt-in. With neither, no read, hint, log line or output may change. A new tunable constant MUST be added to `Tuning::Settings` and to the knob table in `src/tuning/knobs.h`. Tests and the fuzz harness MUST NOT set a tuning field.

## Tests

- Catch2, in `NTFSLibTests/unit-tests/`, one folder per library namespace. A test goes in the folder of the namespace it exercises.
- A header that another test folder includes lives in `<module>/include/ntfs-browser/{test,tool,example}/<module>/`. Those folders are never installed. Tests include such a header as `#include <ntfs-browser/test/<module>/<header>.h>`. A header used by one folder stays beside its `.cpp` and is included with quotes. CMake source lists use the `include/ntfs-browser/...` path.
- Modules with shared headers: `test/` for `fake-image`, `disk-readers`, `fuzz` and the unit test folders `alloc`, `attr`, `corpus`, `device-access`, `efs`, `io`, `lznt1`, `mft`, `support`, `usn`, `volume`. `tool/` for `device-access`, `dir-walk` and `ntfscompare`. `example/` for `ntfsattr`, `ntfsdir`, `ntfsdump`, `ntfsscanbench`, `ntfsundel`.
- Test code lives in `NTFS_BROWSER_NAMESPACE::Test::<Module>` (`Test::FakeImage`, `Test::DiskReaders`, `Test::Fuzz`, `Test::FuzzRegression`, `Test::Support`). Tools live in `::Tool::<Name>` (`Tool::DeviceAccess`, `Tool::DirWalk`, `Tool::NtfsCompare`, `Tool::Benchmark`, `Tool::FuzzGen`). Examples live in `::Example::<Name>` (`Example::NtfsDir`, `Example::NtfsScanBench`). Never `Tests`.
- Inside `Test::*`, a library name MUST be fully qualified (`NTFS_BROWSER_NAMESPACE::Alloc::`). The test namespaces shadow the short names. A `.cpp` MAY alias at global scope: `namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;`.
- Tests MUST NOT touch a real disk or device. They build synthetic images in memory (`NTFSLibTests/tools/fake-image/`, `BuildFakeNtfsImageWithNameTree()`) and serve them through the fake readers in `NTFSLibTests/tools/disk-readers/`. Extend those fakes rather than adding scaffolding. Tests MAY create files under the system temp folder.
- Corpus tests reach their image through `RequireCorpusImage()` / `OpenCorpusImage()`. Without the image they replay the trace that `NtfsFuzzGen` generates (`NTFS_TEST_TRACE_DIR`). `NTFS_BROWSER_FORCE_TRACES=1` replays it even when the image is present. Under `NTFS_BROWSER_REQUIRE_TEST_DATA` an absent image fails. The test skips only when the trace is absent too.
- A test that calls `Log::Configure()` or `SetSinks()` MUST call `InstallCaptureSink()` afterwards.
- A test that touches EFS MUST call `SetEfsKeyProvider(nullptr)` or install a fake. It MUST NOT reach the real certificate store.
- Real-disk checks belong to the manual `NtfsCompare` tool, never to ctest or CI.

## Fuzzing

`FuzzTestcase()` (`NTFSLibTests/fuzz/fuzz-core.cpp`) is shared by `NtfsFuzzer`, `NtfsFuzzerAfl` and the regression test. Never pass `--inject-read-failures` to AFL.

`NtfsFuzzGen` (`NTFSLibTests/fuzz/generator/`) builds every fuzz input and every corpus trace from code. A scenario makes an image, runs the probes on it and keeps the bytes that the library reads, in its current read order. The read order is free: nothing is recorded by hand.

- `NtfsFuzzGen --out DIR` writes all scenarios under `DIR` (traces in `DIR/corpus-traces/`). CMake runs it at build time, with a stamp on the generator, the library and the resource archives. `--seeds DIR` writes the flat inputs only. `--list` prints the names. `--only NAME...` restricts a run. `--verify DIR` compares each scenario with the file of the same name in `DIR`.
- The target `NtfsFuzzSeedCorpus` (not part of ALL) writes `fuzz-generated/afl-seeds`, the seed folder for `afl-fuzz`.
- A scenario that needs LZNT1 decompression or an EFS backend exists only in a build that has it. The regression test skips its file when it is absent.
- A new regression input is a new scenario plus an entry in `fuzzer-expected-messages.h` with the log messages it MUST produce. A test fails when the two lists differ. `NtfsFuzzGenDeterminism` generates twice and compares.
- What code cannot compute (file contents, RAM slack, journal data of the forensic images) is in `generator/resources/*.tar.xz`, one archive per source image. CMake extracts them. `resources/make-corpus-residue.py` makes them and needs the real images at development time only. The generator never reads an image. When the reads of a corpus test change, adapt the probes in `corpus-probes.cpp` and the `.reads` lists with a temporary address-logging patch, as the script's docstring explains.
