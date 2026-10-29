// Scans a synthetic volume through a simulated disk. The time is the one of a
// simulated clock, so a run is instant and repeatable: the benchmark script
// uses it to check that its search finds a known optimum, with no disk.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "bench-tool.h"
#include "fake-ntfs-image.h"
#include "json.h"
#include "memory-disk-reader.h"
#include "throttled-disk-reader.h"

namespace NtfsBenchTool {

namespace {

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

// Version of the layout of the JSON this prints.
constexpr std::uint64_t json_format = 1;

// Slots of the synthetic $MFT, and slots per fragment: 250 per run is the most
// the fixture's run list holds for about 4000 slots.
constexpr std::uint64_t default_records = 4000;
constexpr std::uint64_t fragment_clusters = 250;

// Microseconds in a millisecond, and bytes in a megabyte (decimal, as the
// disks are sold).
constexpr std::uint64_t ns_per_us = 1000;
constexpr std::uint64_t bytes_per_mb = 1000000;

// A hard disk: a long seek, one lane, the same speed alone and together.
Tests::DiskModel HardDisk() {
  Tests::DiskModel model;
  model.seek_ns = 8000000;
  model.per_read_ns = 100000;
  model.single_bytes_per_second = 116 * bytes_per_mb;
  model.max_bytes_per_second = 116 * bytes_per_mb;
  model.lanes = 1;
  return model;
}

}  // namespace

int SimulateCommand(const Arguments& args) {
  bool malformed = false;
  const std::uint64_t records =
      args.Unsigned("records", default_records, malformed);
  const std::string kind = args.Value("model").value_or("ssd");
  Tests::DiskModel model = kind == "hdd" ? HardDisk() : Tests::DiskModel{};
  model.seek_ns =
      args.Unsigned("seek-us", model.seek_ns / ns_per_us, malformed) *
      ns_per_us;
  model.per_read_ns =
      args.Unsigned("per-read-us", model.per_read_ns / ns_per_us, malformed) *
      ns_per_us;
  model.single_bytes_per_second =
      args.Unsigned("single-mbps", model.single_bytes_per_second / bytes_per_mb,
                    malformed) *
      bytes_per_mb;
  model.max_bytes_per_second =
      args.Unsigned("max-mbps", model.max_bytes_per_second / bytes_per_mb,
                    malformed) *
      bytes_per_mb;
  model.lanes = static_cast<std::uint32_t>(
      args.Unsigned("lanes", model.lanes, malformed));
  model.consume_ns_per_mib =
      args.Unsigned("consume-ns-per-mib", model.consume_ns_per_mib, malformed);
  if (malformed || (kind != "ssd" && kind != "hdd") ||
      records <
          static_cast<std::uint64_t>(NTFS_BROWSER_NAMESPACE::Mft::Idx::User) +
              1) {
    std::fprintf(stderr,
                 "simulate: --model is ssd or hdd, --records at least 17\n");
    return 2;
  }

  const std::optional<NTFS_BROWSER_NAMESPACE::VolumeOptions> tuned =
      TunedOptions(args, L"");
  if (!tuned) {
    return 2;
  }
  NTFS_BROWSER_NAMESPACE::VolumeOptions options = *tuned;
  options.include_deleted = args.Has("deleted");
  options.recover_errors = args.Has("recover");

  // The synthetic volume holds damaged records on purpose, and the library
  // says so: a log line costs no simulated time, but would bury the JSON.
  NTFS_BROWSER_NAMESPACE::Log::Config quiet;
  quiet.console_level = NTFS_BROWSER_NAMESPACE::Log::Level::Off;
  static_cast<void>(NTFS_BROWSER_NAMESPACE::Log::Configure(quiet));

  std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithListMftScan(records, fragment_clusters);
  // The image as a file: a volume the tools open without a disk.
  if (const std::optional<std::string> path = args.Value("write-image")) {
    std::ofstream out(
        std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(path->data()), path->size())),
        std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
    if (!out) {
      std::fprintf(stderr, "Cannot write %s\n", path->c_str());
      return 1;
    }
    return 0;
  }

  Tests::SimulatedClock clock;
  auto memory = std::make_unique<Tests::MemoryDiskReader>(std::move(image));
  auto throttled = std::make_unique<Tests::ThrottledDiskReader>(
      std::move(memory), model, clock, options.tuning.reader);
  const Tests::ThrottledDiskReader* const disk = throttled.get();
  NTFS_BROWSER_NAMESPACE::NtfsVolume<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>
      volume(std::move(throttled), options);
  if (!volume.IsVolumeOK()) {
    std::fprintf(stderr, "simulate: the synthetic volume does not open\n");
    return 1;
  }
  NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanOptions scan;
  scan.threads = 1;
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(volume, scan);

  const Tests::ThrottleStats& stats = disk->Stats();
  NtfsDevice::JsonWriter json;
  json.BeginObject();
  json.Field("format", json_format);
  json.Field("model", kind);
  json.Field("records", records);
  json.Field("entries", static_cast<std::uint64_t>(tree.Entries().size()));
  json.Field("sim_ns", clock.NowNs());
  json.Field("disk_reads", stats.disk_reads);
  json.Field("disk_bytes", stats.disk_bytes);
  json.Field("served_reads", stats.served_reads);
  json.Field("unannounced_reads", stats.unannounced_reads);
  json.Field("wait_ns", stats.wait_ns);
  json.Field("wasted_ranges", stats.wasted_ranges);
  json.EndObject();
  return EmitJson(args, json);
}

}  // namespace NtfsBenchTool
