# Critical path of a search

`NtfsScanBench --via-scan` answers two questions about `Search::Scan`: does the disk or the CPU bound a search, and does the CPU work run in parallel. The direct paths of `NtfsScanBench` (no `--via-scan`) time the engines alone. They say nothing about the worker thread of a `Scan`, the match stage or the exchange. `--via-scan` runs the same engines through `Scan::Run()`.

## Method

```
NtfsScanBench --via-scan --engine=all [--name=PATTERN | --regex=PATTERN] [--min-size=N] [--benchmark=run.json] <image>
```

- `--engine` takes `auto`, `mftlist`, `nocache`, `fullcache` or `all`. The run lists nothing. The options of the search are those of `NtfsDir`: `--name` (wildcards), `--regex`, `--match-path`, `--min-size`, `--max-size`.
- The library has two test seams in `src/search/scan-probe.h`. A probe makes the worker of a scan report its wall time, its CPU time, the time it waited for the caller, the time of the match stage, the time of `ScanExchange::Add()` (the lock and the copy) and the entries it saw and delivered. A reader factory makes the scan read through a simulated disk. Without a probe the worker pays one test of a null pointer for each entry, and reads, hints and output do not change.
- T_real is the wall time of the session. T_io is the read log replayed through the production reader. T_cpu is the same scan run again with the file in the cache. R and the critical path follow `ntfs_benchmark.py audit`: R = (T_io + T_cpu - T_real) / min(T_io, T_cpu). The path is I/O when T_cpu is below T_io and T_real is within 10% of T_io.
- Parallelism: cores = CPU of all threads / T_real. Efficiency = cores / threads used. The caller thread sleeps, so a scan that keeps one core busy has an efficiency near 0.5. Read the cores.
- `--benchmark` adds a `scan` member to the summary: the worker, the figures and the verdict.
- `NtfsBench simulate --shape=tree --dirs=30 --levels=3 --files=10 --write-image=tree.img` writes the synthetic volume of the numbers below: 297 930 entries.

Limits of the method:

- T_real is cold only if the file cache was empty. `NtfsBench purge` needs an administrator. The runs below had no such right, so T_io is the time of a replay from the cache: a floor that is too low for a real disk. No run here read a physical disk.
- Windows counts the CPU of a thread in steps of 15.6 ms. A run below 100 ms gives no CPU figure worth reading.
- The probe costs two clock reads in the match stage and two in the exchange for each entry, about 15 ms for 300 000 entries.

## Measures

Release build, 16 cores, synthetic volume of 297 930 entries (365 MB), file in the cache. Medians of three runs. Times in ms. The same command gave times that differed by up to 2x from one batch to the next on this machine: read the shares, not the absolute values.

| Pattern | Engine | Real | IO | CPU | Worker wall | Worker CPU | Match | Exchange | Caller CPU | Cores |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| none | mftlist | 1027 | 124 | 922 | 1022 | 1000 | 8 | 128 | 250 | 1.23 |
| none | nocache | 6579 | 1928 | 7562 | 6577 | 6438 | 21 | 390 | 859 | 1.08 |
| none | fullcache | 8328 | 1174 | 8081 | 8326 | 8109 | 28 | 522 | 1063 | 1.11 |
| `*.dll` | mftlist | 807 | 117 | 779 | 805 | 797 | 25 | 35 | 125 | 1.15 |
| `*.dll` | nocache | 5964 | 1920 | 7786 | 5963 | 5859 | 74 | 251 | 547 | 1.08 |
| `*.dll` | fullcache | 8226 | 1290 | 8114 | 8225 | 8109 | 107 | 374 | 984 | 1.10 |
| regex `^(?:[a-z0-9-]+)+$` | mftlist | 1170 | 113 | 1095 | 1168 | 1125 | 432 | 14 | 16 | 0.98 |
| regex `^(?:[a-z0-9-]+)+$` | nocache | 7529 | 2057 | 8492 | 7528 | 7328 | 1102 | 37 | 47 | 0.98 |
| regex `^(?:[a-z0-9-]+)+$` | fullcache | 9911 | 1913 | 8852 | 9909 | 9328 | 1255 | 46 | 78 | 0.98 |
| regex on the path | mftlist | 929 | 130 | 908 | 928 | 922 | 71 | 0 | 0 | 1.00 |
| regex on the path | nocache | 2736 | 878 | 2746 | 2735 | 2719 | 76 | 0 | 0 | 0.99 |
| regex on the path | fullcache | 2971 | 507 | 2819 | 2971 | 2922 | 88 | 0 | 0 | 0.98 |

The worker never waited (Wait is 0 in every run): the callback is fast, so the pending buffer was never full.

The two forensic images that were tried (`ntfs1-gen2.raw`, `ntfs-lastaccess.raw`) hold 26 entries or fewer. A run takes 2 to 15 ms, below what the CPU counters resolve. They only check that the options work. No finding rests on them.

## Findings

Critical path:

- With the file in the cache, the worker thread is the critical path of every engine: its wall time is 95 to 100% of T_real, and its CPU is 79 to 100% of the CPU of the whole process. T_io is 10 to 32% of T_real, so the CPU hides under no disk time here. The path is CPU, or mixed where R is noisy.
- This says what a fast disk gives. On a real disk the read side decides, and the figures bound it. The mftlist engine reads 95 ranges, 305 MB: at 116 MB/s (a hard disk) the disk needs 2.6 s against 1.0 s of CPU, so the disk bounds it. At 520 MB/s (a SATA SSD) the disk needs 0.6 s and the CPU bounds it. The nocache engine makes 150 564 reads (956 MB) and the fullcache engine 85 958 (403 MB): on a disk that seeks, the reads of both are far above their 6 to 8 s of CPU. This is arithmetic from the read counts, not a measure of a physical disk.

Serial CPU:

- All the CPU work of a search runs on the one worker thread, and only the caller's polling runs beside it. The process keeps 0.98 to 1.23 cores busy in every run.
- The match stage is on the worker. A wildcard costs 1 to 3% of the worker's time. The heavy regex costs 37% on mftlist (432 of 1168 ms) and 15% on the walks. A regex on the path costs 3 to 8%.
- The exchange (the lock and the copy of each entry) costs 12% of the worker on mftlist with no pattern (128 ms for 297 930 entries), and 6% on the walks. It falls with the number of entries that pass the pattern.
- The rest of the worker's time is the engine: parse and read. It is 86% of the mftlist run with no pattern. The MftList engine built its tree on one thread here: no pool thread reported. `MftTree` chooses its threads from the volume, and this volume is small for it.
- The caller thread uses 0.25 to 1.06 s of CPU for the unfiltered runs, 12 to 13% of the run. It runs beside the worker, so it is not on the critical path while a second core is free.

Recommendation (nothing changed in this task):

- Look at the CPU of the caller thread first: the callback only counts, so the cost is in `Poll()` and `Release()` of the exchange. It is one core of 1.1 for 300 000 entries.
- The exchange takes the lock for each entry. Giving the entries to it in batches would cut the 6 to 12%. The gain is small beside the engine's own time.
- The match stage is the one serial part that can dominate: a regex may take a third of the run. Running it on the caller or on a pool would use the cores that are idle. A wildcard does not need it.
- The engine's own time is the bulk. The MftList scan on one thread (1.0 s for 298 000 records) is where a second core would help most. Check the thread count that `MftTree` chooses for the scan engine.
- Measure again on a cold disk, with `NtfsBench purge` run first and an image on a physical drive, before any change that trades CPU for reads.
