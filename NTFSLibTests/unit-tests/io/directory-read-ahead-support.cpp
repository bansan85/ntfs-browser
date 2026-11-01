#include "directory-read-ahead-support.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfsdir-core.h"
#include "test-log-sink.h"
#include "throttled-disk-reader.h"

namespace NTFS_BROWSER_NAMESPACE::Tests::ReadAhead {

namespace {

// What the log of a disk read says: the read ahead reads in other places and
// other sizes than the listing does, and those lines follow the reads.
constexpr std::string_view read_line_marker = "Successfully read";

// Bytes at the start of an index block.
constexpr std::string_view block_magic = "INDX";

// Bytes at the start of a file record.
constexpr std::string_view record_magic = "FILE";

// A directory node under parent.
FakeTreeNode Directory(std::wstring name, std::size_t parent) {
  return {.links = {{.name = std::move(name), .parent = parent}},
          .directory = true};
}

// A file node under parent.
FakeTreeNode File(std::wstring name, std::size_t parent) {
  return {.links = {{.name = std::move(name), .parent = parent}}};
}

// A node called name, a POSIX name: NTFS lets two of them differ only by case.
FakeTreeNode PosixName(std::wstring name, std::size_t parent, bool directory) {
  return {
      .links = {{.name = std::move(name), .parent = parent, .name_space = 0}},
      .directory = directory};
}

// Decimal text of value, at least two digits.
std::wstring Number(std::size_t value) {
  std::wstring text = std::to_wstring(value);
  return text.size() < 2 ? L"0" + text : text;
}

// The offset in image of file record record, going by the volume's own $MFT.
ULONGLONG RecordOffset(const std::vector<BYTE>& image, ULONGLONG record) {
  NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<MemoryDiskReader>(image));
  REQUIRE(volume.IsVolumeOK());
  return volume.GetMFTAddr() + (record * volume.GetFileRecordSize());
}

// Overwrites the magic that starts the bytes of image at offset.
void SpoilMagic(std::vector<BYTE>& image, std::size_t offset) {
  std::memset(&image.at(offset), 'X', record_magic.size());
}

// The offsets in image where an index block starts.
std::vector<std::size_t> IndexBlockOffsets(const std::vector<BYTE>& image) {
  std::vector<std::size_t> offsets;
  auto at = image.begin();
  while (true) {
    at = std::search(at, image.end(), block_magic.begin(), block_magic.end());
    if (at == image.end()) {
      return offsets;
    }
    offsets.push_back(static_cast<std::size_t>(at - image.begin()));
    at += block_magic.size();
  }
}

// A tree in which the same names differ only by case.
std::vector<FakeTreeNode> CaseTwinTree() {
  std::vector<FakeTreeNode> nodes;
  nodes.push_back(PosixName(L"Docs", name_tree_root, true));
  nodes.push_back(PosixName(L"docs", name_tree_root, true));
  nodes.push_back(PosixName(L"Report.txt", 0, false));
  nodes.push_back(PosixName(L"report.txt", 0, false));
  nodes.push_back(PosixName(L"Sub", 1, true));
  nodes.push_back(PosixName(L"sub", 1, true));
  nodes.push_back(File(L"inner.txt", 4));
  nodes.push_back(File(L"INNER.TXT", 5));
  return nodes;
}

// A tree with freed directories and files whose entries stayed in the index.
std::vector<FakeTreeNode> DeletedTree() {
  std::vector<FakeTreeNode> nodes;
  nodes.push_back(Directory(L"live", name_tree_root));
  nodes.push_back(File(L"live.txt", 0));
  FakeTreeNode gone = Directory(L"gone", name_tree_root);
  gone.in_use = false;
  gone.keep_entries = true;
  nodes.push_back(gone);
  FakeTreeNode gone_file = File(L"gone.txt", 0);
  gone_file.in_use = false;
  gone_file.keep_entries = true;
  nodes.push_back(gone_file);
  nodes.push_back(Directory(L"after", name_tree_root));
  nodes.push_back(File(L"after.txt", 4));
  return nodes;
}

// The tree of WideTree(), with the record of one directory spoiled.
std::vector<BYTE> TreeWithUnreadableDirectory() {
  const std::vector<FakeTreeNode> nodes = WideTree(8);
  std::vector<BYTE> image = BuildFakeNtfsImageWithNameTree(nodes);
  // The third directory of the root: its entry stays, its record is lost.
  const std::size_t victim = 2 * 4;
  SpoilMagic(image, RecordOffset(image, name_tree_first_record + victim));
  return image;
}

// The tree of BigDirectoryTree(), with one of its index blocks spoiled.
std::vector<BYTE> TreeWithDamagedBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithNameTree(BigDirectoryTree(4));
  const std::vector<std::size_t> blocks = IndexBlockOffsets(image);
  REQUIRE(blocks.size() > 4);
  SpoilMagic(image, blocks.at(blocks.size() / 2));
  return image;
}

// Reads back the file that holds a listing, and removes it.
std::string ReadAndRemove(std::FILE* file, const std::filesystem::path& path) {
  std::fflush(file);
  std::rewind(file);
  std::string text;
  std::array<char, 4096> chunk{};
  while (const std::size_t got =
             std::fread(chunk.data(), 1, chunk.size(), file)) {
    text.append(chunk.data(), got);
  }
  std::fclose(file);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  return text;
}

// The lines of log, but those about disk reads.
std::string WithoutReadLines(const std::string& log) {
  std::istringstream lines(log);
  std::string kept;
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find(read_line_marker) == std::string::npos) {
      kept += line;
      kept += '\n';
    }
  }
  return kept;
}

// The options of a run: the read ahead set on top of options.
VolumeOptions WithReadAhead(VolumeOptions options, std::size_t dirs,
                            std::uint64_t bytes) {
  options.tuning.walk.read_ahead_dirs = dirs;
  options.tuning.walk.read_ahead_bytes = bytes;
  return options;
}

// Lists volume recursively, and gathers the text and the log.
template <Cache::Strategy S>
Run ListVolume(NtfsVolume<S>& volume, CountingDiskReader& reads) {
  reads.Reset();
  (void)TakeCapturedLog();

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("read-ahead-listing-" + std::to_string(std::random_device{}()) + ".txt");
  std::FILE* file = std::fopen(path.string().c_str(), "w+b");
  REQUIRE(file != nullptr);

  Run run;
  run.exit_code = ListFrom<S>(volume, L"image", L"", L"image", true, file);
  run.text = ReadAndRemove(file, path);
  run.log = WithoutReadLines(TakeCapturedLog());
  run.calls = reads.Calls();
  return run;
}

}  // namespace

std::vector<FakeTreeNode> WideTree(std::size_t directories) {
  std::vector<FakeTreeNode> nodes;
  for (std::size_t index = 0; index < directories; index++) {
    // Four nodes for each directory: it, two files, one subdirectory with a
    // file, in that order of record.
    const std::size_t dir = nodes.size();
    nodes.push_back(Directory(L"dir" + Number(index), name_tree_root));
    nodes.push_back(File(L"a" + Number(index) + L".txt", dir));
    nodes.push_back(Directory(L"sub" + Number(index), dir));
    nodes.push_back(File(L"b" + Number(index) + L".txt", dir + 2));
  }
  return nodes;
}

std::vector<FakeTreeNode> GroupedTree(std::size_t groups,
                                      std::size_t per_group) {
  std::vector<FakeTreeNode> nodes;
  for (std::size_t group = 0; group < groups; group++) {
    const std::size_t parent = nodes.size();
    nodes.push_back(Directory(L"group" + Number(group), name_tree_root));
    for (std::size_t index = 0; index < per_group; index++) {
      const std::size_t dir = nodes.size();
      nodes.push_back(Directory(L"d" + Number(index), parent));
      nodes.push_back(File(L"f.txt", dir));
    }
  }
  return nodes;
}

std::vector<FakeTreeNode> BigDirectoryTree(std::size_t directories) {
  std::vector<FakeTreeNode> nodes;
  for (std::size_t index = 0; index < directories; index++) {
    const std::size_t dir = nodes.size();
    nodes.push_back(Directory(L"big" + Number(index), name_tree_root));
    for (std::size_t file = 0; file < big_directory_files; file++) {
      nodes.push_back(File(L"f" + Number(file) + L".txt", dir));
    }
    nodes.push_back(Directory(L"child" + Number(index), dir));
    nodes.push_back(File(L"leaf.txt", nodes.size() - 1));
  }
  return nodes;
}

std::vector<TreeFixture> Fixtures() {
  std::vector<TreeFixture> fixtures;
  const auto add = [&fixtures](std::string name,
                               const std::vector<FakeTreeNode>& nodes) {
    fixtures.push_back(
        {std::move(name), BuildFakeNtfsImageWithNameTree(nodes)});
  };
  add("wide", WideTree(40));
  add("grouped", GroupedTree(4, 12));
  add("deep",
      MakeNameChain(
          24, [](std::size_t level) { return L"level" + Number(level); },
          L"bottom.txt"));
  add("big directories", BigDirectoryTree(6));
  add("case twins", CaseTwinTree());
  add("freed entries", DeletedTree());
  fixtures.push_back({"unreadable directory", TreeWithUnreadableDirectory()});
  fixtures.push_back({"damaged block", TreeWithDamagedBlock()});
  fixtures.push_back({"directory loop", BuildFakeNtfsImageWithDirectoryLoop()});
  fixtures.push_back({"orphaned directory loop",
                      BuildFakeNtfsImageWithOrphanedDirectoryLoop()});
  fixtures.push_back(
      {"orphaned blocks", BuildFakeNtfsImageWithOrphanedIndexBlocks()});
  return fixtures;
}

std::vector<VolumeOptions> AllOptions() {
  return {VolumeOptions{}, VolumeOptions{.recover_errors = true},
          VolumeOptions{.include_deleted = true},
          VolumeOptions{.include_deleted = true, .recover_errors = true}};
}

template <Cache::Strategy S>
Run ListRecursively(const std::vector<BYTE>& image, VolumeOptions options,
                    std::size_t dirs, std::uint64_t bytes) {
  auto counting = std::make_unique<CountingDiskReader>(
      std::make_unique<MemoryDiskReader>(image));
  CountingDiskReader& reads = *counting;
  NtfsVolume<S> volume(std::move(counting),
                       WithReadAhead(options, dirs, bytes));
  REQUIRE(volume.IsVolumeOK());
  return ListVolume(volume, reads);
}

template <Cache::Strategy S>
Run ListWithDefaultTuning(const std::vector<BYTE>& image,
                          const VolumeOptions& options) {
  auto counting = std::make_unique<CountingDiskReader>(
      std::make_unique<MemoryDiskReader>(image));
  CountingDiskReader& reads = *counting;
  NtfsVolume<S> volume(std::move(counting), options);
  REQUIRE(volume.IsVolumeOK());
  return ListVolume(volume, reads);
}

DiskModel HardDiskModel() {
  DiskModel model;
  model.seek_ns = 8000000;
  model.per_read_ns = 100000;
  model.single_bytes_per_second = 116'000'000;
  model.max_bytes_per_second = 116'000'000;
  model.lanes = 1;
  return model;
}

template <Cache::Strategy S>
TimedRun ListOnSimulatedDisk(const std::vector<BYTE>& image,
                             VolumeOptions options, std::size_t dirs,
                             std::uint64_t bytes, const DiskModel& model) {
  SimulatedClock clock;
  auto throttled = std::make_unique<ThrottledDiskReader>(
      std::make_unique<MemoryDiskReader>(image), model, clock,
      WithReadAhead(options, dirs, bytes).tuning.reader);
  const ThrottledDiskReader& disk = *throttled;
  auto counting = std::make_unique<CountingDiskReader>(std::move(throttled));
  CountingDiskReader& reads = *counting;
  NtfsVolume<S> volume(std::move(counting),
                       WithReadAhead(options, dirs, bytes));
  REQUIRE(volume.IsVolumeOK());

  // Only the listing is timed, not the opening of the volume.
  const std::uint64_t opened_ns = clock.NowNs();
  const ThrottleStats opened = disk.Stats();
  TimedRun timed;
  timed.run = ListVolume(volume, reads);
  timed.elapsed_ns = clock.NowNs() - opened_ns;
  timed.disk = disk.Stats();
  timed.disk.disk_reads -= opened.disk_reads;
  timed.disk.disk_bytes -= opened.disk_bytes;
  timed.disk.served_reads -= opened.served_reads;
  timed.disk.unannounced_reads -= opened.unannounced_reads;
  timed.disk.wait_ns -= opened.wait_ns;
  timed.disk.wasted_ranges -= opened.wasted_ranges;
  return timed;
}

std::string FirstDifference(const std::string& left, const std::string& right) {
  if (left == right) {
    return {};
  }
  // Lines of context before the first difference.
  constexpr std::size_t context_lines = 6;
  std::size_t at = 0;
  while (at < left.size() && at < right.size() && left[at] == right[at]) {
    at++;
  }
  std::size_t from = at;
  for (std::size_t lines = 0; from > 0 && lines <= context_lines; from--) {
    if (left[from - 1] == '\n') {
      lines++;
    }
  }
  const auto tail = [from](const std::string& text) {
    return text.substr(from, 400);
  };
  return "left:\n" + tail(left) + "\nright:\n" + tail(right);
}

MftSpan MftSpanOf(const std::vector<BYTE>& image) {
  NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<MemoryDiskReader>(image));
  REQUIRE(volume.IsVolumeOK());
  const ULONGLONG first = volume.GetMFTAddr();
  return {.first = first,
          .end =
              first + (volume.GetRecordsCount() * volume.GetFileRecordSize())};
}

std::vector<std::uint32_t>
    SectorReadCounts(const std::vector<CountingDiskReader::Call>& calls,
                     std::size_t image_bytes) {
  constexpr ULONGLONG sector = 512;
  std::vector<std::uint32_t> counts((image_bytes / sector) + 1, 0);
  for (const CountingDiskReader::Call& call : calls) {
    if (call.kind != CountingDiskReader::Kind::Read) {
      continue;
    }
    for (ULONGLONG at = call.offset / sector;
         at * sector < call.offset + call.length; at++) {
      if (at < counts.size()) {
        counts.at(static_cast<std::size_t>(at))++;
      }
    }
  }
  return counts;
}

template Run ListRecursively<Cache::Strategy::NoCache>(
    const std::vector<BYTE>& image, VolumeOptions options, std::size_t dirs,
    std::uint64_t bytes);
template Run ListRecursively<Cache::Strategy::FullCache>(
    const std::vector<BYTE>& image, VolumeOptions options, std::size_t dirs,
    std::uint64_t bytes);
template Run ListWithDefaultTuning<Cache::Strategy::NoCache>(
    const std::vector<BYTE>& image, const VolumeOptions& options);
template Run ListWithDefaultTuning<Cache::Strategy::FullCache>(
    const std::vector<BYTE>& image, const VolumeOptions& options);
template TimedRun ListOnSimulatedDisk<Cache::Strategy::NoCache>(
    const std::vector<BYTE>& image, VolumeOptions options, std::size_t dirs,
    std::uint64_t bytes, const DiskModel& model);
template TimedRun ListOnSimulatedDisk<Cache::Strategy::FullCache>(
    const std::vector<BYTE>& image, VolumeOptions options, std::size_t dirs,
    std::uint64_t bytes, const DiskModel& model);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::ReadAhead
