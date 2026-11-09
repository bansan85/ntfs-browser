// Scans a synthetic volume through a simulated disk. The time is the one of a
// simulated clock, so a run is instant and repeatable: the benchmark script
// uses it to check that its search finds a known optimum, with no disk.

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/disk-readers/throttled-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/device-access/json.h>

#include "bench-tool.h"
#include "disk-reader.h"
#include "log/log.h"
#include "mft/idx.h"
#include "mft/tree.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::Benchmark {

namespace {

namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;

// Version of the layout of the JSON this prints.
constexpr std::uint64_t json_format = 1;

// Slots of the synthetic $MFT, and slots per fragment: 250 per run is the most
// the fixture's run list holds for about 4000 slots.
constexpr std::uint64_t default_records = 4000;
constexpr std::uint64_t fragment_clusters = 250;

// The shape "tree" of --shape, meant for --write-image, to measure a search on
// names: 30 folders in each of 3 levels, 10 files in each of the last: 270 000
// files.
constexpr std::uint64_t default_tree_dirs = 30;
constexpr std::uint64_t default_tree_levels = 3;
constexpr std::uint64_t default_tree_files = 10;

// Microseconds in a millisecond, and bytes in a megabyte (decimal, as the
// disks are sold).
constexpr std::uint64_t ns_per_us = 1000;
constexpr std::uint64_t bytes_per_mb = 1000000;

// A hard disk: a long seek, one lane, the same speed alone and together.
TestDiskReaders::DiskModel HardDisk() {
  TestDiskReaders::DiskModel model;
  model.seek_ns = 8000000;
  model.per_read_ns = 100000;
  model.single_bytes_per_second = 116 * bytes_per_mb;
  model.max_bytes_per_second = 116 * bytes_per_mb;
  model.lanes = 1;
  return model;
}

// The disk model of the command line: the kind's defaults, then each --seek-us,
// --per-read-us, --single-mbps, --max-mbps, --lanes and --consume-ns-per-mib.
TestDiskReaders::DiskModel ModelFromArguments(const Arguments& args,
                                              const std::string& kind,
                                              bool& malformed) {
  TestDiskReaders::DiskModel model =
      kind == "hdd" ? HardDisk() : TestDiskReaders::DiskModel{};
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
  return model;
}

// A tree of levels levels of folders: dirs folders under the root, dirs folders
// in each of those, and so on. Each folder of the last level holds files files,
// so dirs ^ levels * files files in all. A folder holds only a few dozen names,
// so the shape grows by levels. The names cycle through four extensions and
// the sizes (resident, so below 700) through a range, so that a pattern or a
// size bound picks some of them.
std::vector<BYTE> BuildTreeImage(std::uint64_t dirs, std::uint64_t levels,
                                 std::uint64_t files) {
  // Extensions the names cycle through, and the modulus and step of the sizes.
  constexpr std::array<const wchar_t*, 4> extensions{L".txt", L".dll", L".log",
                                                     L".dat"};
  constexpr std::uint64_t size_modulus = 700;
  constexpr std::uint64_t size_step = 37;
  std::vector<TestFakeImage::FakeTreeNode> nodes;
  // The node index of the first folder of the level before, and how many
  // folders that level has.
  std::uint64_t previous_first = 0;
  std::uint64_t previous_count = 1;
  for (std::uint64_t level = 0; level < levels; ++level) {
    const std::uint64_t first = nodes.size();
    const std::uint64_t count = previous_count * dirs;
    for (std::uint64_t index = 0; index < count; ++index) {
      TestFakeImage::FakeTreeNode node{
          .links = {{.name = L"d" + std::to_wstring(index % dirs)}},
          .directory = true};
      if (level > 0) {
        node.links.front().parent =
            static_cast<std::size_t>(previous_first + (index / dirs));
      }
      nodes.push_back(std::move(node));
    }
    previous_first = first;
    previous_count = count;
  }
  for (std::uint64_t leaf = 0; leaf < previous_count; ++leaf) {
    for (std::uint64_t file = 0; file < files; ++file) {
      const std::uint64_t number = (leaf * files) + file;
      nodes.push_back(
          {.links = {{.name = L"r" + std::to_wstring(leaf) + L"-" +
                              std::to_wstring(file) +
                              extensions.at(number % extensions.size()),
                      .parent =
                          static_cast<std::size_t>(previous_first + leaf)}},
           .data_size =
               static_cast<DWORD>((number * size_step) % size_modulus)});
    }
  }
  return TestFakeImage::BuildFakeNtfsImageWithNameTree(nodes);
}

// Writes the image to the file named by path. False when it cannot be written.
bool WriteImageFile(const std::string& path, const std::vector<BYTE>& image) {
  std::ofstream out(
      std::filesystem::path(std::u8string(
          reinterpret_cast<const char8_t*>(path.data()), path.size())),
      std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(image.data()),
            static_cast<std::streamsize>(image.size()));
  return static_cast<bool>(out);
}

// The synthetic volume holds damaged records on purpose, and the library says
// so: a log line costs no simulated time, but would bury the JSON.
void SilenceLibraryLog() {
  NTFS_BROWSER_NAMESPACE::Log::Config quiet;
  quiet.console_level = NTFS_BROWSER_NAMESPACE::Log::Level::Off;
  static_cast<void>(NTFS_BROWSER_NAMESPACE::Log::Configure(quiet));
}

// Writes the counters of the simulated disk, as fields of the open object.
void WriteThrottleStats(Tool::DeviceAccess::JsonWriter& json,
                        const TestDiskReaders::ThrottleStats& stats) {
  json.Field("disk_reads", stats.disk_reads);
  json.Field("disk_bytes", stats.disk_bytes);
  json.Field("served_reads", stats.served_reads);
  json.Field("unannounced_reads", stats.unannounced_reads);
  json.Field("wait_ns", stats.wait_ns);
  json.Field("wasted_ranges", stats.wasted_ranges);
}

// What the command line asks for, once read and checked.
struct SimulationSetup {
  std::uint64_t records = 0;
  std::uint64_t tree_dirs = 0;
  std::uint64_t tree_levels = 0;
  std::uint64_t tree_files = 0;
  // The shape of the volume: --shape=tree, else the list of $MFT fragments.
  bool tree_shape = false;
  std::string kind;
  TestDiskReaders::DiskModel model;
  NTFS_BROWSER_NAMESPACE::VolumeOptions options;
  // The file of --write-image. Set: the command writes the image and stops.
  std::optional<std::string> write_image;
};

// Reads and checks the options of the command. Nothing, after saying why on
// stderr, when one is malformed or out of range.
std::optional<SimulationSetup> ParseSimulation(const Arguments& args) {
  SimulationSetup setup;
  bool malformed = false;
  setup.records = args.Unsigned("records", default_records, malformed);
  setup.tree_dirs = args.Unsigned("dirs", default_tree_dirs, malformed);
  setup.tree_levels = args.Unsigned("levels", default_tree_levels, malformed);
  setup.tree_files = args.Unsigned("files", default_tree_files, malformed);
  setup.kind = args.Value("model").value_or("ssd");
  setup.model = ModelFromArguments(args, setup.kind, malformed);
  if (malformed || (setup.kind != "ssd" && setup.kind != "hdd") ||
      setup.records <
          static_cast<std::uint64_t>(NTFS_BROWSER_NAMESPACE::Mft::Idx::User) +
              1) {
    std::fprintf(stderr,
                 "simulate: --model is ssd or hdd, --records at least 17\n");
    return std::nullopt;
  }

  const std::optional<NTFS_BROWSER_NAMESPACE::VolumeOptions> tuned =
      TunedOptions(args, L"");
  if (!tuned) {
    return std::nullopt;
  }
  setup.options = *tuned;
  setup.options.include_deleted = args.Has("deleted");
  setup.options.recover_errors = args.Has("recover");
  setup.tree_shape = args.Value("shape").value_or("list") == "tree";
  setup.write_image = args.Value("write-image");
  return setup;
}

// Scans the image through the simulated disk and prints the JSON. 1 when the
// volume does not open.
int RunSimulation(const Arguments& args, const SimulationSetup& setup,
                  std::vector<BYTE> image) {
  TestDiskReaders::SimulatedClock clock;
  auto memory =
      std::make_unique<TestDiskReaders::MemoryDiskReader>(std::move(image));
  auto throttled = std::make_unique<TestDiskReaders::ThrottledDiskReader>(
      std::move(memory), setup.model, clock, setup.options.tuning.reader);
  const TestDiskReaders::ThrottledDiskReader* const disk = throttled.get();
  NTFS_BROWSER_NAMESPACE::NtfsVolume<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>
      volume(std::move(throttled), setup.options);
  if (!volume.IsVolumeOK()) {
    std::fprintf(stderr, "simulate: the synthetic volume does not open\n");
    return 1;
  }
  NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanOptions scan;
  scan.threads = 1;
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(volume, scan);

  const TestDiskReaders::ThrottleStats& stats = disk->Stats();
  Tool::DeviceAccess::JsonWriter json;
  json.BeginObject();
  json.Field("format", json_format);
  json.Field("model", setup.kind);
  json.Field("records", setup.records);
  json.Field("entries", static_cast<std::uint64_t>(tree.Entries().size()));
  json.Field("sim_ns", clock.NowNs());
  WriteThrottleStats(json, stats);
  json.EndObject();
  return EmitJson(args, json);
}

}  // namespace

int SimulateCommand(const Arguments& args) {
  const std::optional<SimulationSetup> setup = ParseSimulation(args);
  if (!setup) {
    return 2;
  }

  SilenceLibraryLog();

  std::vector<BYTE> image =
      setup->tree_shape ? BuildTreeImage(setup->tree_dirs, setup->tree_levels,
                                         setup->tree_files)
                        : TestFakeImage::BuildFakeNtfsImageWithListMftScan(
                              setup->records, fragment_clusters);
  // The image as a file: a volume the tools open without a disk.
  if (setup->write_image) {
    if (!WriteImageFile(*setup->write_image, image)) {
      std::fprintf(stderr, "Cannot write %s\n", setup->write_image->c_str());
      return 1;
    }
    return 0;
  }
  return RunSimulation(args, *setup, std::move(image));
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::Benchmark
