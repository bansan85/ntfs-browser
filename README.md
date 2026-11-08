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
- `NtfsMftList C:` lists every file of a volume. `--deleted` adds deleted files, and `--recover` salvages damaged entries and reads the change journal (`$UsnJrnl`), which names files the `$MFT` lost. `--threads=N` sets the threads that parse the `$MFT` and format the listing. `--clusters` counts the clusters of each deleted or damaged file by condition (intact, rewritten, bad...), `--clusters=map` also prints where they lie in the file, and `--verify-data[=sample|full]` reads the clusters presumed intact to tell zeros and NTFS metadata from data.
- `NtfsDir`, `NtfsDir2` and `NtfsMftList` also take the benchmark options (see Benchmark and tuning).
- `NtfsUndel C:` lists the deleted files of a volume. `NtfsUndel C: --output D:\recovered --all` recovers them, and `--record N` or `--name "*.docx"` picks some. The output folder MUST be on another drive than the one that is read. `--include-doubtful` also recovers a file whose data may belong to another file.

On Linux, `NtfsDir` and `NtfsMftList` take a device or an image file. Reading a device needs read access to it.

- `NtfsDir /dev/sda1/Windows` or `NtfsDir ./disk.img/Windows` lists a folder.
- `NtfsMftList /dev/sda1` or `NtfsMftList ./disk.img` lists every file of a volume, with the same options as on Windows.
- `NtfsUndel /dev/sda1 --output /mnt/backup/recovered --all` recovers the deleted files into a folder on another drive.
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

Some tests read real forensic images. `python .github/scripts/fetch-test-data.py` downloads them. Without them, those tests replay traces that the build generates, so `ctest` passes without any download.

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

## Benchmark and tuning

The tools can time themselves, and the constants of the read path can be tuned for one disk. Both are off by default: with neither, the library reads exactly what it always read.

### What the tools can do

These options work on `NtfsMftList`, `NtfsDir` and `NtfsDir2`:

- `--benchmark[=<path>]` times the run. It writes a JSON summary at `<path>` (`ntfs-benchmark.json` by default) and the log of every read at `<path>.readlog`. The summary has the phases (open, pass1, pass2, flush, sweep, check, link, journal, walk, print) with their wall time and CPU time, the CPU time of each thread, the reads by kind with their size histogram, the sizes the disk saw, the reads in flight, the bytes read ahead and wasted, the seeks, and the serial tail after the last read. Each read that the reader serves is classed: `io` (the caller waited for the disk), `window` (the read was done, and the read-ahead caps held others back), `ready` (done, nothing held back) or `plan` (never announced, so the caller waited).
- `--capture=<path>` writes every range the run read into a sparse file of the volume's size. Run the tool on that image, with the image in the file cache, and the time is that of the CPU alone.
- `--stop-after=<phase>` ends the run after a phase, to time that phase alone.
- `--profile=<path>` sets the tunable constants from a profile (below). `--disk=<key>` takes the section of another disk. `--tune=<name>[@M1..M4]=<value>` sets one constant, after the profile. `--media=ssd|hdd|unknown` says the kind of device to plan the reads for, instead of the one the device reports. An image file takes the kind of the volume that hosts it, so a copy of a volume kept on a hard disk reads as a hard disk; the summary writes the kind the run used as `run.media_kind`.

Write the listing of a scanned volume to another drive. A listing of 290 MB on the scanned drive changes what is measured.

### NtfsBench and the script

`NtfsBench` is the measuring tool. `NtfsBench replay <readlog> <target>` replays the reads of a run through the production reader, as they were and with every read announced up front: that is the time the disk alone costs. `NtfsBench diskbench <target>` measures the disk in the zone of its `$MFT`: sequential 4 MiB reads and random 4 KiB and 64 KiB reads, at queue depths 1 to 32. `NtfsBench knobs` lists the tunable constants. `purge`, `wake`, `disk-id` and `sample` serve the script. `simulate` scans a synthetic volume through a simulated disk, in microseconds of simulated time.

`NTFSLibTests/tools/benchmark/ntfs_benchmark.py` (Python 3.12, standard library only; an administrator console on Windows) drives it:

```
python NTFSLibTests/tools/benchmark/ntfs_benchmark.py audit --bin-dir build/static --out-dir out --volume c: --volume e:
python NTFSLibTests/tools/benchmark/ntfs_benchmark.py search --bin-dir build/static --out-dir out --volume c: --patterns M1,M2 --profile-out profile.json --validate
python NTFSLibTests/tools/benchmark/ntfs_benchmark.py --self-test
```

`audit` runs NtfsMftList in its four modes, and ntfsdir on a subtree, a big directory and a deep path, with the cold cache each time. For each it gives T_real (the tool), T_io (the replay), T_cpu (the tool on the captured image, run with `--media=` set to the kind the real run used) and R = (T_io + T_cpu - T_real) / min(T_io, T_cpu). I/O is the critical path when T_cpu is below T_io and T_real stays within 10 % of T_io. It also samples the disk every 100 ms, sweeps `--threads`, and writes `results.json` and `report.md`. `search` finds the best value of each constant of a read pattern, and writes the profile. `--self-test` checks the searcher on a simulated disk with a known best value. The workflow `benchmark.yml` runs the self-test, an audit of `c:` and a short search on a Windows runner, when started by hand.

### Tunable constants and the profile

`VolumeOptions::tuning` holds the constants. Their defaults are the values the library had before the knobs, but `cache.block_bytes`, which is 4 KiB and was 64 KiB, and `walk.read_ahead_dirs`, which is 64 and was 0 (below). With 4 KiB blocks a FullCache walk reads and holds what it needs, and a read that spans blocks reads the missing ones together (`cache.max_run_bytes`, 1 MiB at most for each disk read). They fall in three families: depth of the read ahead (it changes when bytes are read, never which), shape of the reads (it changes which bytes are read: window, gap, hole, cache block, memory budgets) and CPU and memory. The guard rails against forged input are never tunable. Shape constants are set by an explicit option only, but one: on a hard disk read through FullCache, the probes of a directory walk read across a wider hole of `$MFT` (`probe.wide_gap_bytes`), which the device itself decides (a seek penalty it reports, never a measure).

`walk.read_ahead_dirs` and `walk.read_ahead_bytes` (family A, pattern M3) turn on the read ahead of a recursive `ntfsdir -r` listing. On by default (`walk.read_ahead_dirs` is 64): the records and the first level of index blocks of the next directories are read together, sorted by place on the disk and announced first, and held (up to `walk.read_ahead_bytes`) until the listing reaches them. The listing prints and logs what it always did. `--tune=walk.read_ahead_dirs=0` turns it off. A NoCache listing (`NtfsDir2`) also keeps the last runs of `$MFT` that its probes read, `walk.record_window_bytes` (family B, pattern M3, 4 MiB), because NoCache keeps nothing and neighbouring directories share the clusters of `$MFT`: `c:\Windows\System32 -r` takes 2.7 s instead of 3.6 and the whole system volume 130 s instead of 210-240. A FullCache listing has the block cache and keeps none. 0 turns it off. `walk.record_memo_entries` (family B, pattern M3, 65 536) keeps, for the records of those runs, the few bytes a probe reads of each (32 bytes a record), so that the neighbours of a probed record are found long after the window dropped them. The read ahead also reads every level of the index of a directory, down to the leaves, in sorted batches, where the walk read them one at a time: `c:\Windows\System32 -r` takes 2.4 s instead of 2.6 with NtfsDir2 (2.1 instead of 2.2 with NtfsDir), the whole system volume 111 s instead of 138, and `e:\Qt\6.12.0 -r` on a hard disk 17.3 s instead of 20.3 (16.4 instead of 18.5 with NtfsDir). When the window is half held but the next directory of the listing is not, that one directory is read alone: 16.5 s instead of 17.3 on `e:\Qt\6.12.0 -r` with NtfsDir2, and 105 s instead of 109 on the whole system volume, with the same peak memory. `walk.read_ahead_runs` (family A, pattern M3, 64) announces the reads that the probes of the held directories will make (runs of `$MFT`, planned from the held index) on a second, standing queue of the disk reader, that the withdrawals of the walk do not touch: only a hint, no byte more is read. It is off at 0, and on a hard disk (a device that reports a seek penalty), which takes `walk.read_ahead_runs_rotational` instead (0 by default). On a SATA SSD the lane takes `c:\Windows\System32 -r` from 2.1 to 1.8 s with NtfsDir2 and from 2.4 to 2.0 s with NtfsDir, and the whole system volume from 106 to 95 s and from 82 to 76 s, with the same listings. `ntfs_benchmark.py search --patterns M3 --subtree <folder>` searches these knobs, starting from the default and trying the values above and below it in steps of two.

A profile is a JSON file with one section per disk, keyed by `"<vendor> <product>|<serial>"`. `common` applies to every read pattern, `M1` (large sequential reads), `M2` (small scattered reads, announced), `M3` (small dependent reads) and `M4` (large data reads) override it:

```json
{
  "format": 1,
  "disks": {
    "Verbatim Vi560|493633408990154": {
      "label": "c: SATA SSD",
      "common": { "mft.window_bytes": 4194304, "reader.max_reads": 32 },
      "M2": { "reader.max_reads": 16, "mft.max_announced_records": 64 }
    }
  }
}
```

An unknown key, a value out of range or a constant that a pattern does not serve is refused with a message that names it. `NtfsBench knobs` prints every name with its family, patterns, bounds, default and `memory_unit_bytes` (the bytes of memory one unit of the value holds, 0 for a gap or a threshold), which the search uses to prefer the value that costs the least memory on a plateau.

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
- 2026-11 — `NtfsAttr`, `NtfsDump` and `NtfsUndel` are console tools on Windows and Linux, MFC is gone from the build

## Credits and licence

Based on [An NTFS Parser Lib](https://www.codeproject.com/Articles/81456/An-NTFS-Parser-Lib) from CodeProject. The project is licensed under BSD-3c. The original web page is archived in `documentation/old`.
