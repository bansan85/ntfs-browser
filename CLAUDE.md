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
- Feature options `NTFS_BROWSER_ENABLE_DECOMPRESSION`, `..._EFS_CRYPTOPP`, `..._EFS_BCRYPT` compile code in or out. Test the macro with `#if` in a `.cpp` or `src/*.h`. A public header MUST NOT test it. Pair the BCrypt macro with `_WIN32`.
- `NtfsDir2` and `NtfsFuzzer` are Windows-only. The library, unit tests, the console tools (`NtfsAttr`, `NtfsDump`, `NtfsUndel`, `NtfsDir`, `NtfsMftList`, `NtfsCompare`) and `NtfsFuzzerAfl` also build on Linux. There is no MFC in the build.
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
- Every public function in `include/ntfs-browser/*.h` gets a purpose comment. So does a private method defined in a `.cpp`.
- Every constant gets a comment: what it is for, and why that value.
- A `TEST_CASE` or `SECTION` MUST NOT get a comment. Its name is the description.
- A boolean condition with four or more distinct elements across more than one kind of check gets one comment line saying what it means. Skip it if a log call in the same branch already says so.

## Architecture rules

- **Namespaces.** Write `NTFS_BROWSER_NAMESPACE::`, never the literal name. Only `IDiskReader`, `NtfsVolume`, `IndexEntry`, `Filename` and `VolumeOptions` sit in the root. Everything else goes in a domain sub-namespace (`Attr`, `Data`, `Io`, `Mft`, `Cache`, `Alloc`, `Log`, `Efs`, ...). A file `<prefix>-*` lives in folder `<prefix>/`. Namespaces follow the domain, not the C++ entity kind.
- **Strategy templates.** Core classes are templated on `Cache::Strategy` (`NoCache`, `FullCache`). A template whose only parameter is the strategy MUST NOT define any function in a header: declare it, define it in the `.cpp`, and end the `.cpp` with explicit instantiations for both strategies. `.github/scripts/check-strategy-templates.py` enforces it. Under `NoCache`, attribute data is a view into a short-lived buffer. Callers MUST NOT assume it outlives the next read. Under `FullCache`, keep the `Cache::View`, not a bare span.
- **Shared builds export symbol by symbol.** A public symbol carries `NTFS_BROWSER_EXPORT`. An internal symbol that tests reach through `src/` carries `NTFS_BROWSER_EXPORT_TESTS_ONLY`. A member function template repeats the macro on each explicit instantiation.
- **One class, one file pair.** Every `.cpp` in `src/` has a `.h` of the same name, in `src/` or in `include/ntfs-browser/`. A class gets its own header and its own `.cpp`.
- **Pimpl.** `NtfsVolume<S>`, `FileRecord<S>` and `MftTree` keep all data members and private methods in a private nested `Impl`. A public header MUST NOT declare them. `Impl` is defined in a `src/` header (`ntfs-volume-impl.h`, `io/file-record-impl.h`) or in the `.cpp` (`mft/tree.cpp`). Another class reaches that state through a method of the owner.
- **No `friend`** anywhere in `include/`, `src/` or `NTFSLibTests/`. Make a narrow public method instead. `check-no-friend.py` enforces it.
- **Logging.** Everything goes through `Log::Trace/Debug/Info/Warn/Error` in `src/log/ntfs-common.h`. spdlog MUST NOT appear in `include/ntfs-browser/*.h` (except the opt-in `log/spdlog-adapter.h`) or in any `src/*.h` the tests include. Neither the library nor its logging is thread safe: `Configure()`, `SetSinks()` and every emitted message come from one thread. A worker thread captures its log and the owning thread replays it.
- **Crypto++, spdlog and `<windows.h>`** MUST NOT appear in a public header, except `win-types.h`, which includes `<windows.h>` on Windows because public structs hold `FILETIME` by value.
- **Paths.** Code that opens a user-given path MUST go through `Io::ToExtendedLengthPath()`. Tests that create or list paths over 260 units MUST extend them too.
- **Names** are raw UTF-16 units. Collate through `$UpCase`, never the locale. A map or set keyed by a name MUST NOT fold case. Print names as UTF-8 (WTF-8 for unpaired surrogates) with `Utf::WideToUtf8()`.
- **Strict vs recovering.** `VolumeOptions::recover_errors` chooses between rejecting a damaged item and salvaging it. `include_deleted` decides which healthy items show. Call `Log::Recoverable()` only for a condition that strict mode would reject.
- **Instrumentation and tuning** are opt-in. With neither, no read, hint, log line or output may change: recorded traces and the fuzz corpus check it. A new tunable constant MUST be added to `Tuning::Settings` and to the knob table in `include/ntfs-browser/tuning/knobs.h`. Tests and the fuzz harness MUST NOT set a tuning field.

## Tests

- Catch2, in `NTFSLibTests/unit-tests/`, one folder per library namespace. A test goes in the folder of the namespace it exercises.
- A header that another test folder includes lives in `<module>/include/ntfs-browser/{test,tool,example}/<module>/`. Those folders are never installed. Tests include such a header as `#include <ntfs-browser/test/<module>/<header>.h>`. A header used by one folder stays beside its `.cpp` and is included with quotes. CMake source lists use the `include/ntfs-browser/...` path.
- Modules with public headers: `test/` for `fake-image`, `disk-readers`, `fuzz` and the unit test folders `alloc`, `attr`, `corpus`, `device-access`, `efs`, `io`, `lznt1`, `mft`, `support`, `usn`, `volume`. `tool/` for `device-access` and `ntfscompare`. `example/` for `ntfsattr`, `ntfsdir`, `ntfsdump`, `ntfsundel`.
- Test code lives in `NTFS_BROWSER_NAMESPACE::Test::<Module>` (`Test::FakeImage`, `Test::DiskReaders`, `Test::Fuzz`, `Test::FuzzRegression`, `Test::Support`). Tools live in `::Tool::<Name>` (`Tool::DeviceAccess`, `Tool::NtfsCompare`, `Tool::Benchmark`). Examples live in `::Example::<Name>` (`Example::NtfsDir`, `Example::NtfsMftList`). Never `Tests`.
- Inside `Test::*`, a library name MUST be fully qualified (`NTFS_BROWSER_NAMESPACE::Alloc::`). The test namespaces shadow the short names. A `.cpp` MAY alias at global scope: `namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;`.
- Tests MUST NOT touch a real disk or device. They build synthetic images in memory (`NTFSLibTests/tools/fake-image/`, `BuildFakeNtfsImageWithNameTree()`) and serve them through the fake readers in `NTFSLibTests/tools/disk-readers/`. Extend those fakes rather than adding scaffolding. Tests MAY create files under the system temp folder.
- Corpus tests reach their image through `RequireCorpusImage()` / `OpenCorpusImage()`. Recorded traces live in `NTFSLibTests/fuzz/data/corpus-traces/`. After any change to the read path, record them again with `NTFS_BROWSER_RECORD_TRACES=1` and the images present.
- A test that calls `Log::Configure()` or `SetSinks()` MUST call `InstallCaptureSink()` afterwards.
- A test that touches EFS MUST call `SetEfsKeyProvider(nullptr)` or install a fake. It MUST NOT reach the real certificate store.
- Real-disk checks belong to the manual `NtfsCompare` tool, never to ctest or CI.

## Fuzzing

`FuzzTestcase()` (`NTFSLibTests/fuzz/fuzz-core.cpp`) is shared by `NtfsFuzzer`, `NtfsFuzzerAfl` and the regression test. Saved inputs in `NTFSLibTests/fuzz/data/` replay by read order, not by address. So the read order of `FuzzTestcase()` MUST NOT depend on the build, and a new probe MUST run after the existing ones. Never pass `--inject-read-failures` to AFL.
