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

### Command-line tools

On Windows:

- `NtfsDir "C:\Windows"` lists a folder. `NtfsDir C:\image.img\Windows` lists a folder of an image file. `NtfsDir --recursive C:` (or `-r`) lists a folder and every folder below it. `--deleted` adds deleted files, marked with `*`, and `--recover` salvages damaged directory data.
- `NtfsMftList C:` lists every file of a volume. `--deleted` adds deleted files, and `--recover` salvages damaged entries and reads the change journal (`$UsnJrnl`), which names files the `$MFT` lost. `--threads=N` sets the threads that parse the `$MFT`. `--clusters` counts the clusters of each deleted or damaged file by condition (intact, rewritten, bad...), `--clusters=map` also prints where they lie in the file, and `--verify-data[=sample|full]` reads the clusters presumed intact to tell zeros and NTFS metadata from data.
- `NtfsUndel` is a graphical tool that finds and recovers deleted files.

On Linux, `NtfsDir` and `NtfsMftList` take a device or an image file. Reading a device needs read access to it.

- `NtfsDir /dev/sda1/Windows` or `NtfsDir ./disk.img/Windows` lists a folder.
- `NtfsMftList /dev/sda1` or `NtfsMftList ./disk.img` lists every file of a volume, with the same options as on Windows.
- `NtfsCompare /dev/sda1/Windows` compares the library with the operating system. If the partition is not mounted, it only compares the library's own listings with each other.

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

`python .github/scripts/fetch-test-data.py external` fetches a selection of NTFS files (`$MFT`, `$J`, `$I30`, boot sectors, `$LogFile`, Sleuth Kit listings) from three projects: [dfir_ntfs](https://github.com/msuhanov/dfir_ntfs), [MFT](https://github.com/EricZimmerman/MFT) and [DFIRArtifactMuseum](https://github.com/AndrewRathbun/DFIRArtifactMuseum). It clones each one shallowly at a pinned revision, keeps only the selected paths (about 384 MiB downloaded, 650 MiB on disk), and checks every file against its SHA-256. It needs `git` and, for the `.7z` files, `7zz`, `7z`, `7za` or `7zr` on the `PATH`. Without 7-Zip it skips only those files. Nothing from these projects is committed. Their tests are skipped when a file is absent, and fail under `-DNTFS_BROWSER_REQUIRE_TEST_DATA=ON`.

CMake looks for the corpora under `NTFS_BROWSER_TEST_DATA_DIR` (default `test-data/`). A cache variable, or an environment variable of the same name, points one corpus elsewhere:

| Variable | Default | Content |
|---|---|---|
| `NTFS_BROWSER_TEST_DFTT_DIR` | `<data>/dftt` | DFTT images |
| `NTFS_BROWSER_TEST_NPS_NTFS1_DIR` | `<data>/nps-2009-ntfs1` | NPS ntfs1-gen2 raw image |
| `NTFS_BROWSER_TEST_NTFS_SAMPLES_DIR` | `<data>/ntfs-samples` | ntfs-samples raw images |
| `NTFS_BROWSER_TEST_DFIR_NTFS_DIR` | `<data>/dfir-ntfs` | dfir_ntfs files, under `test_data/` |
| `NTFS_BROWSER_TEST_ZIMMERMAN_MFT_DIR` | `<data>/zimmerman-mft` | MFT project files (none are taken today) |
| `NTFS_BROWSER_TEST_DFIR_MUSEUM_DIR` | `<data>/dfir-artifact-museum` | DFIRArtifactMuseum files, under `Windows/NTFSArtifacts/` |

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
- 2026-10 — `NtfsDir` and `NtfsMftList` run on Linux, on a device or an image file
- 2026-10 — Custom log destinations and an opt-in spdlog adapter
- 2026-10 — Cluster allocation from `$Bitmap`
- 2026-10 — Bad clusters from `$BadClus`
- 2026-10 — Change journal (`$UsnJrnl`) in recovery mode

## Credits and licence

Based on [An NTFS Parser Lib](https://www.codeproject.com/Articles/81456/An-NTFS-Parser-Lib) from CodeProject. The project is licensed under BSD-3c. The original web page is archived in `documentation/old`.
