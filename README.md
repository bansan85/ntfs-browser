# ntfs-browser

A C++20 Fast NTFS browser.

Reading a volume needs administrator rights.

**Disclaimer:** Please read these points first:

- The original project was taken over from codeproject.
- I have no knowledge of the NTFS or MFT on-disk format on the author's side.
- Large changes (refactoring, tests, new features) were then made by an AI assistant (Claude), based on the available documentation and its own knowledge of NTFS.
- The code is 100% vibe coded, and no human has reviewed it.
- Large changes are still in progress, and the public API is not stable yet.
- Use at your own risk, and check results against another tool before relying on them.

## Features

- Browse the folders of an NTFS volume.
- Read files, including compressed files and encrypted files. Encrypted files need the certificate that encrypted them.
- Show deleted files, if you ask for them.
- Recover what can be read from a damaged volume, if you ask for it.
- Read-only: the library never writes to the disk.

## Usage

### Command-line tools (Windows)

- `NtfsDir "C:\Windows"` lists a folder.
- `NtfsMftList C:` lists every file of a volume. `--deleted` adds deleted files, and `--recover` salvages damaged entries.
- `NtfsUndel` is a graphical tool that finds and recovers deleted files.

### As a library (C++)

```cpp
#include <cstdio>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>

// Called once for each entry of the directory.
void CountEntry(const NtfsBrowser::IndexEntryView& /*entry*/, void* context) {
  ++*static_cast<int*>(context);
}

int main() {
  using Strategy = NtfsBrowser::Cache::Strategy;

  NtfsBrowser::NtfsVolume<Strategy::FullCache> volume(L'C');
  if (!volume.IsVolumeOK()) {
    return 1;
  }

  // Record 5 is the root directory.
  NtfsBrowser::Io::FileRecord<Strategy::FullCache> root(volume);
  if (!root.ParseFileRecord(5) || !root.ParseAttrs()) {
    return 1;
  }

  int count = 0;
  root.TraverseSubEntries(CountEntry, &count);
  std::printf("%d entries in the root directory\n", count);
  return 0;
}
```

## Installation

You need:

- Windows 10 or 11 with Visual Studio 2022 or 2026, or Linux with GCC or Clang.
- CMake 3.25 or newer.
- Git.

Clone the repository with its submodules, which hold the third-party libraries:

```
git clone --recurse-submodules https://github.com/bansan85/ntfs-browser
```

If you cloned without `--recurse-submodules`, run `git submodule update --init --recursive`.

## Build and test

On Windows, with the CMake presets:

```
cmake --preset static
cmake --build --preset static --parallel
ctest --test-dir build/static -C Debug
```

On Linux:

```
cmake -S . -B build-linux
cmake --build build-linux --parallel
ctest --test-dir build-linux
```

Some tests read real forensic images. `python .github/scripts/fetch-test-data.py` downloads them. Without them, those tests replay recorded traces, so `ctest` passes without any download.

To leave features out, or to change the build, add options to the configure step:

- `-DBUILD_SHARED_LIBS=ON` builds a DLL instead of a static library.
- `-DNTFS_BROWSER_ENABLE_DECOMPRESSION=OFF` removes compressed-file support.
- `-DNTFS_BROWSER_ENABLE_EFS_CRYPTOPP=OFF` and `-DNTFS_BROWSER_ENABLE_EFS_BCRYPT=OFF` remove the encryption backends (BCrypt is Windows only).
- `-DNTFS_BROWSER_BUILD_MFC_APPS=OFF` skips the graphical tools.

## Documentation

Each public class and function is described in the headers under `include/ntfs-browser/`.

## Contributing

- Format the code before committing: `uv run bash ./.github/scripts/format.sh`.
- Commit messages follow Conventional Commits, with a title of 50 characters at most.
- [CLAUDE.md](CLAUDE.md) holds the project rules for code, tests and logging.

## Changelog

- 2022-08 — Import of the CodeProject [An NTFS Parser Lib](https://www.codeproject.com/Articles/81456/An-NTFS-Parser-Lib), without changes
- 2022-08 — First C++ cleanup, CMake build, CI with clang-format and clang-tidy
- 2022-09 — Disk read cache (`FULL_CACHE`) and first unit tests
- 2023-10 — Portability fixes: shared library on Windows, clang-cl, `Cache::Strategy` as template parameter
- 2023-11 — Full path support
- 2023-11 → 2026-08 — Pause, no commits
- 2026-08 — Start of work with Claude Code: bug fixes and new features from here on
- 2026-09 — Hardening against corrupted volumes
- 2026-09 — AFL fuzzer and Linux port
- 2026-09 — Deleted files recovery
- 2026-09 — Damaged data recovery
- 2026-09 — LZNT1 decompression
- 2026-09 — EFS decryption (AES, 3DES, DESX)
- 2026-09 — Integration tests on forensic images, `NtfsCompare` metadata cross-check
- 2026-10 — Sanitizer builds and 32-bit builds
- 2026-10 — Static analysis fixes and refactoring, namespaces
- 2026-10 — Bounded `FullCache` memory
- 2026-10 — Faster `$MFT` reads and optional read-ahead

## Credits and licence

Based on [An NTFS Parser Lib](https://www.codeproject.com/Articles/81456/An-NTFS-Parser-Lib) from CodeProject. The project is licensed under BSD-3c. The original web page is archived in `documentation/old`.
