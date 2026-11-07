#include "directory-read-ahead-support.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/example/ntfsdir/ntfsdir-core.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/directory-read-ahead.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/io/record-standing.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/disk-readers/throttled-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "io/directory-read-ahead-testing.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

struct HeaderCommon;

}  // namespace Attr
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Tests::ReadAhead {

VerifiedLaneZone::VerifiedLaneZone() noexcept
    : mismatches_before_(Io::LaneZoneMismatchesForTests()) {
  Io::SetVerifyLaneZoneForTests(true);
}

VerifiedLaneZone::~VerifiedLaneZone() {
  Io::SetVerifyLaneZoneForTests(false);
  CHECK(Io::LaneZoneMismatchesForTests() == mismatches_before_);
}

namespace {

// What the log of a disk read says: the read ahead reads in other places and
// other sizes than the listing does, and those lines follow the reads.
constexpr std::string_view read_line_marker = "Successfully read";

// Bytes at the start of an index block.
constexpr std::string_view block_magic = "INDX";

// Bytes at the start of a file record.
constexpr std::string_view record_magic = "FILE";

// A directory node under parent.
Test::FakeImage::FakeTreeNode Directory(std::wstring name, std::size_t parent) {
  return {.links = {{.name = std::move(name), .parent = parent}},
          .directory = true};
}

// A file node under parent.
Test::FakeImage::FakeTreeNode File(std::wstring name, std::size_t parent) {
  return {.links = {{.name = std::move(name), .parent = parent}}};
}

// A node called name, a POSIX name: NTFS lets two of them differ only by case.
Test::FakeImage::FakeTreeNode PosixName(std::wstring name, std::size_t parent,
                                        bool directory) {
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
      std::make_unique<Test::DiskReaders::MemoryDiskReader>(image));
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
std::vector<Test::FakeImage::FakeTreeNode> CaseTwinTree() {
  std::vector<Test::FakeImage::FakeTreeNode> nodes;
  nodes.push_back(PosixName(L"Docs", Test::FakeImage::name_tree_root, true));
  nodes.push_back(PosixName(L"docs", Test::FakeImage::name_tree_root, true));
  nodes.push_back(PosixName(L"Report.txt", 0, false));
  nodes.push_back(PosixName(L"report.txt", 0, false));
  nodes.push_back(PosixName(L"Sub", 1, true));
  nodes.push_back(PosixName(L"sub", 1, true));
  nodes.push_back(File(L"inner.txt", 4));
  nodes.push_back(File(L"INNER.TXT", 5));
  return nodes;
}

// A tree with freed directories and files whose entries stayed in the index.
std::vector<Test::FakeImage::FakeTreeNode> DeletedTree() {
  std::vector<Test::FakeImage::FakeTreeNode> nodes;
  nodes.push_back(Directory(L"live", Test::FakeImage::name_tree_root));
  nodes.push_back(File(L"live.txt", 0));
  Test::FakeImage::FakeTreeNode gone =
      Directory(L"gone", Test::FakeImage::name_tree_root);
  gone.in_use = false;
  gone.keep_entries = true;
  nodes.push_back(gone);
  Test::FakeImage::FakeTreeNode gone_file = File(L"gone.txt", 0);
  gone_file.in_use = false;
  gone_file.keep_entries = true;
  nodes.push_back(gone_file);
  nodes.push_back(Directory(L"after", Test::FakeImage::name_tree_root));
  nodes.push_back(File(L"after.txt", 4));
  return nodes;
}

// The tree of WideTree(), with the record of one directory spoiled.
std::vector<BYTE> TreeWithUnreadableDirectory() {
  const std::vector<Test::FakeImage::FakeTreeNode> nodes = WideTree(8);
  std::vector<BYTE> image =
      Test::FakeImage::BuildFakeNtfsImageWithNameTree(nodes);
  // The third directory of the root: its entry stays, its record is lost.
  const std::size_t victim = 2 * 4;
  SpoilMagic(
      image,
      RecordOffset(image, Test::FakeImage::name_tree_first_record + victim));
  return image;
}

// The tree of BigDirectoryTree(), with one of its index blocks spoiled.
std::vector<BYTE> TreeWithDamagedBlock() {
  std::vector<BYTE> image =
      Test::FakeImage::BuildFakeNtfsImageWithNameTree(BigDirectoryTree(4));
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
Run ListVolume(NtfsVolume<S>& volume,
               Test::DiskReaders::CountingDiskReader& reads) {
  reads.Reset();
  (void)TakeCapturedLog();
  const std::uint64_t recovered_before = Log::Detail::RecoveredCount();

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("read-ahead-listing-" + std::to_string(std::random_device{}()) + ".txt");
  std::FILE* file = std::fopen(path.string().c_str(), "w+b");
  REQUIRE(file != nullptr);

  Run run;
  run.exit_code = ListFrom<S>(
      volume, {.label = L"image", .path = L"", .root = L"image"}, true, file);
  run.text = ReadAndRemove(file, path);
  run.log = WithoutReadLines(TakeCapturedLog());
  run.calls = reads.Calls();
  run.recovered = Log::Detail::RecoveredCount() - recovered_before;
  return run;
}

}  // namespace

std::vector<Test::FakeImage::FakeTreeNode> WideTree(std::size_t directories) {
  std::vector<Test::FakeImage::FakeTreeNode> nodes;
  for (std::size_t index = 0; index < directories; index++) {
    // Four nodes for each directory: it, two files, one subdirectory with a
    // file, in that order of record.
    const std::size_t dir = nodes.size();
    nodes.push_back(
        Directory(L"dir" + Number(index), Test::FakeImage::name_tree_root));
    nodes.push_back(File(L"a" + Number(index) + L".txt", dir));
    nodes.push_back(Directory(L"sub" + Number(index), dir));
    nodes.push_back(File(L"b" + Number(index) + L".txt", dir + 2));
  }
  return nodes;
}

std::vector<Test::FakeImage::FakeTreeNode> SpacedTree(std::size_t directories,
                                                      std::size_t files,
                                                      std::size_t ballast) {
  std::vector<Test::FakeImage::FakeTreeNode> nodes;
  for (std::size_t index = 0; index < directories; index++) {
    const std::size_t dir = nodes.size();
    nodes.push_back(
        Directory(L"dir" + Number(index), Test::FakeImage::name_tree_root));
    for (std::size_t file = 0; file < files; file++) {
      nodes.push_back(File(L"f" + Number(file) + L".txt", dir));
    }
    for (std::size_t freed = 0; freed < ballast; freed++) {
      Test::FakeImage::FakeTreeNode node =
          File(L"b" + Number(freed) + L".txt", dir);
      node.in_use = false;
      nodes.push_back(std::move(node));
    }
  }
  return nodes;
}

std::vector<Test::FakeImage::FakeTreeNode> GroupedTree(std::size_t groups,
                                                       std::size_t per_group) {
  std::vector<Test::FakeImage::FakeTreeNode> nodes;
  for (std::size_t group = 0; group < groups; group++) {
    const std::size_t parent = nodes.size();
    nodes.push_back(
        Directory(L"group" + Number(group), Test::FakeImage::name_tree_root));
    for (std::size_t index = 0; index < per_group; index++) {
      const std::size_t dir = nodes.size();
      nodes.push_back(Directory(L"d" + Number(index), parent));
      nodes.push_back(File(L"f.txt", dir));
    }
  }
  return nodes;
}

std::vector<Test::FakeImage::FakeTreeNode>
    BigDirectoryTree(std::size_t directories) {
  std::vector<Test::FakeImage::FakeTreeNode> nodes;
  for (std::size_t index = 0; index < directories; index++) {
    const std::size_t dir = nodes.size();
    nodes.push_back(
        Directory(L"big" + Number(index), Test::FakeImage::name_tree_root));
    for (std::size_t file = 0; file < big_directory_files; file++) {
      nodes.push_back(File(L"f" + Number(file) + L".txt", dir));
    }
    nodes.push_back(Directory(L"child" + Number(index), dir));
    nodes.push_back(File(L"leaf.txt", nodes.size() - 1));
  }
  return nodes;
}

std::vector<Test::FakeImage::FakeTreeNode>
    ScatteredTree(std::size_t directories, std::size_t files,
                  std::size_t ballast) {
  std::vector<Test::FakeImage::FakeTreeNode> nodes;
  for (std::size_t index = 0; index < directories; index++) {
    const std::size_t dir = nodes.size();
    nodes.push_back(
        Directory(L"dir" + Number(index), Test::FakeImage::name_tree_root));
    for (std::size_t file = 0; file < files; file++) {
      nodes.push_back(File(L"f" + Number(file) + L".txt", dir));
      for (std::size_t freed = 0; freed < ballast; freed++) {
        Test::FakeImage::FakeTreeNode node =
            File(L"b" + Number(freed) + L".txt", dir);
        node.in_use = false;
        nodes.push_back(std::move(node));
      }
    }
  }
  return nodes;
}

namespace {

// Adds the subdirectories of the directory at parent, and theirs, down to
// levels more levels.
void AddBranches(std::vector<Test::FakeImage::FakeTreeNode>& nodes,
                 std::size_t parent, std::size_t fanout, std::size_t levels) {
  if (levels == 0) {
    return;
  }
  for (std::size_t branch = 0; branch < fanout; branch++) {
    const std::size_t dir = nodes.size();
    nodes.push_back(Directory(L"d" + Number(branch), parent));
    nodes.push_back(File(L"f.txt", dir));
    AddBranches(nodes, dir, fanout, levels - 1);
  }
}

}  // namespace

std::vector<Test::FakeImage::FakeTreeNode> BranchingTree(std::size_t fanout,
                                                         std::size_t depth) {
  std::vector<Test::FakeImage::FakeTreeNode> nodes;
  AddBranches(nodes, Test::FakeImage::name_tree_root, fanout, depth);
  return nodes;
}

std::vector<Test::FakeImage::FakeTreeNode> LinkedTree(std::size_t directories,
                                                      std::size_t files) {
  std::vector<Test::FakeImage::FakeTreeNode> nodes;
  for (std::size_t index = 0; index < directories; index++) {
    const std::size_t dir = nodes.size();
    nodes.push_back(
        Directory(L"dir" + Number(index), Test::FakeImage::name_tree_root));
    for (std::size_t file = 0; file < files; file++) {
      Test::FakeImage::FakeTreeNode node =
          File(L"f" + Number(file) + L".txt", dir);
      if (file % 2 == 0) {
        // A hard link, which collates far from the first name.
        node.links.push_back({.name = L"z" + Number(file) + L".txt",
                              .parent = dir,
                              .name_space = 1});
      } else {
        // The DOS alias of the name, right beside it.
        node.links.push_back({.name = L"F" + Number(file) + L"~1.TXT",
                              .parent = dir,
                              .name_space = 2});
      }
      nodes.push_back(std::move(node));
    }
  }
  return nodes;
}

std::vector<TreeFixture> Fixtures() {
  std::vector<TreeFixture> fixtures;
  const auto add =
      [&fixtures](std::string name,
                  const std::vector<Test::FakeImage::FakeTreeNode>& nodes) {
        fixtures.push_back(
            {std::move(name),
             Test::FakeImage::BuildFakeNtfsImageWithNameTree(nodes)});
      };
  add("wide", WideTree(40));
  add("grouped", GroupedTree(4, 12));
  add("deep",
      Test::FakeImage::MakeNameChain(
          24, [](std::size_t level) { return L"level" + Number(level); },
          L"bottom.txt"));
  add("big directories", BigDirectoryTree(6));
  add("case twins", CaseTwinTree());
  add("freed entries", DeletedTree());
  fixtures.push_back({"unreadable directory", TreeWithUnreadableDirectory()});
  fixtures.push_back({"damaged block", TreeWithDamagedBlock()});
  fixtures.push_back({"directory loop",
                      Test::FakeImage::BuildFakeNtfsImageWithDirectoryLoop()});
  fixtures.push_back(
      {"orphaned directory loop",
       Test::FakeImage::BuildFakeNtfsImageWithOrphanedDirectoryLoop()});
  fixtures.push_back(
      {"orphaned blocks",
       Test::FakeImage::BuildFakeNtfsImageWithOrphanedIndexBlocks()});
  fixtures.push_back(
      {"deep index chain",
       Test::FakeImage::BuildFakeNtfsImageWithDeepIndexBlockChain()});
  return fixtures;
}

std::vector<VolumeOptions> AllOptions() {
  std::vector<VolumeOptions> options;
  options.push_back(VolumeOptions{});
  options.push_back(VolumeOptions{.recover_errors = true});
  options.push_back(VolumeOptions{.include_deleted = true});
  options.push_back(
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  return options;
}

template <Cache::Strategy S>
Run ListRecursively(const std::vector<BYTE>& image, VolumeOptions options,
                    std::size_t dirs, std::uint64_t bytes,
                    Tuning::MediaKind kind) {
  auto memory = std::make_unique<Test::DiskReaders::MemoryDiskReader>(image);
  memory->SetMediaKind(kind);
  auto counting = std::make_unique<Test::DiskReaders::CountingDiskReader>(
      std::move(memory));
  Test::DiskReaders::CountingDiskReader& reads = *counting;
  NtfsVolume<S> volume(std::move(counting),
                       WithReadAhead(options, dirs, bytes));
  REQUIRE(volume.IsVolumeOK());
  return ListVolume(volume, reads);
}

template <Cache::Strategy S>
Run ListWithDefaultTuning(const std::vector<BYTE>& image,
                          const VolumeOptions& options) {
  auto counting = std::make_unique<Test::DiskReaders::CountingDiskReader>(
      std::make_unique<Test::DiskReaders::MemoryDiskReader>(image));
  Test::DiskReaders::CountingDiskReader& reads = *counting;
  NtfsVolume<S> volume(std::move(counting), options);
  REQUIRE(volume.IsVolumeOK());
  return ListVolume(volume, reads);
}

namespace {

// The subdirectories a listing of one directory collects, in index order.
struct LaneChildren {
  std::vector<ULONGLONG> references;
};

// Whether a listing enters the entry, as the visitor of NtfsDir does: a
// directory with a Win32 name, not reused, and not a metadata file.
bool IsListedSubdirectory(const IndexEntryView& entry,
                          Io::RecordStanding standing) {
  if (entry.GetFileReference() < static_cast<ULONGLONG>(Mft::Idx::User)) {
    return false;
  }
  if (!entry.IsWin32Name() || entry.GetFilename().empty()) {
    return false;
  }
  return entry.IsDirectory() && standing != Io::RecordStanding::Reused;
}

// TraverseSubEntriesWithStanding callback of ListForLane(): collects the
// subdirectories the way the visitor of NtfsDir does.
void CollectChildren(const IndexEntryView& entry, Io::RecordStanding standing,
                     void* context) {
  if (IsListedSubdirectory(entry, standing)) {
    static_cast<LaneChildren*>(context)->references.push_back(
        entry.GetFileReference());
  }
}

// Keeps every attribute: a callback only has to be there.
void KeepAttribute(const Attr::HeaderCommon& /*header*/, bool& discard) {
  discard = false;
}

// The first window directories at the top of the stack that are not listed
// yet, the top first.
std::vector<ULONGLONG>
    UpcomingDirectories(const std::vector<ULONGLONG>& pending,
                        const std::unordered_set<ULONGLONG>& listed,
                        std::size_t window) {
  std::vector<ULONGLONG> upcoming;
  for (auto next = pending.rbegin();
       next != pending.rend() && upcoming.size() < window; ++next) {
    if (!listed.contains(*next)) {
      upcoming.push_back(*next);
    }
  }
  return upcoming;
}

// Pops the stack down to the next directory that parses, and enters it. A
// directory that is skipped or does not parse is released. False when the
// stack ran out.
template <Cache::Strategy S>
bool EnterNextDirectory(Io::FileRecord<S>& fr, Io::DirectoryReadAhead<S>& ahead,
                        std::vector<ULONGLONG>& pending,
                        std::unordered_set<ULONGLONG>& listed) {
  while (!pending.empty()) {
    const ULONGLONG next = pending.back();
    pending.pop_back();
    if (listed.contains(next)) {
      ahead.Release(next);
      continue;
    }
    if (fr.ParseFileRecord(next) && fr.ParseAttrs()) {
      listed.insert(next);
      return true;
    }
    ahead.Release(next);
  }
  return false;
}

// The listing of ListTree() of NtfsDir, without its output: the same walk of
// the stack and the same calls on ahead.
template <Cache::Strategy S>
void WalkStack(Io::FileRecord<S>& fr, Io::DirectoryReadAhead<S>& ahead) {
  std::vector<ULONGLONG> pending;
  std::unordered_set<ULONGLONG> listed;
  if (const std::optional<ULONGLONG> start = fr.GetFileReference()) {
    listed.insert(*start);
  }
  ahead.Attach(fr);
  ahead.Begin();
  do {
    LaneChildren children;
    fr.TraverseSubEntriesWithStanding(CollectChildren, &children);
    if (const std::optional<ULONGLONG> current = fr.GetFileReference()) {
      ahead.Release(*current);
    }
    pending.insert(pending.end(), children.references.rbegin(),
                   children.references.rend());
    if (ahead.Enabled()) {
      ahead.Plan(UpcomingDirectories(pending, listed, ahead.Window()));
    }
  } while (EnterNextDirectory(fr, ahead, pending, listed));
}

}  // namespace

template <Cache::Strategy S>
LaneRun ListForLane(const std::vector<BYTE>& image,
                    const VolumeOptions& options, Tuning::MediaKind kind,
                    bool raw_callback) {
  auto memory = std::make_unique<Test::DiskReaders::MemoryDiskReader>(image);
  memory->SetMediaKind(kind);
  auto counting = std::make_unique<Test::DiskReaders::CountingDiskReader>(
      std::move(memory));
  Test::DiskReaders::CountingDiskReader& reads = *counting;
  NtfsVolume<S> volume(std::move(counting), options);
  REQUIRE(volume.IsVolumeOK());
  if (raw_callback) {
    REQUIRE(volume.InstallAttrRawCB(Attr::Type::Data, &KeepAttribute));
  }
  volume.SetReadPattern(Tuning::Pattern::DependentSmall);
  Io::FileRecord<S> fr(volume);
  fr.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(fr.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(fr.ParseAttrs());
  reads.Reset();

  LaneRun run;
  {
    Io::DirectoryReadAhead<S> ahead(volume);
    WalkStack(fr, ahead);
    run.stats = ahead.GetStats();
  }
  for (const Test::DiskReaders::CountingDiskReader::Call& call :
       reads.Calls()) {
    const bool lane =
        call.kind ==
            Test::DiskReaders::CountingDiskReader::Kind::PrefetchStanding ||
        call.kind ==
            Test::DiskReaders::CountingDiskReader::Kind::CancelStanding;
    (lane ? run.lane : run.others).push_back(call);
  }
  return run;
}

Test::DiskReaders::DiskModel HardDiskModel() {
  Test::DiskReaders::DiskModel model;
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
                             std::uint64_t bytes,
                             const SimulatedDevice& device) {
  Test::DiskReaders::SimulatedClock clock;
  auto memory = std::make_unique<Test::DiskReaders::MemoryDiskReader>(image);
  memory->SetMediaKind(device.kind);
  auto throttled = std::make_unique<Test::DiskReaders::ThrottledDiskReader>(
      std::move(memory), device.model, clock,
      WithReadAhead(options, dirs, bytes).tuning.reader);
  const Test::DiskReaders::ThrottledDiskReader& disk = *throttled;
  auto counting = std::make_unique<Test::DiskReaders::CountingDiskReader>(
      std::move(throttled));
  Test::DiskReaders::CountingDiskReader& reads = *counting;
  NtfsVolume<S> volume(std::move(counting),
                       WithReadAhead(options, dirs, bytes));
  REQUIRE(volume.IsVolumeOK());

  // Only the listing is timed, not the opening of the volume.
  const std::uint64_t opened_ns = clock.NowNs();
  const Test::DiskReaders::ThrottleStats opened = disk.Stats();
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
      std::make_unique<Test::DiskReaders::MemoryDiskReader>(image));
  REQUIRE(volume.IsVolumeOK());
  const ULONGLONG first = volume.GetMFTAddr();
  return {.first = first,
          .end =
              first + (volume.GetRecordsCount() * volume.GetFileRecordSize())};
}

std::vector<std::uint32_t> SectorReadCounts(
    const std::vector<Test::DiskReaders::CountingDiskReader::Call>& calls,
    std::size_t image_bytes) {
  constexpr ULONGLONG sector = 512;
  std::vector<std::uint32_t> counts((image_bytes / sector) + 1, 0);
  for (const Test::DiskReaders::CountingDiskReader::Call& call : calls) {
    if (call.kind != Test::DiskReaders::CountingDiskReader::Kind::Read) {
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
    std::uint64_t bytes, Tuning::MediaKind kind);
template Run ListRecursively<Cache::Strategy::FullCache>(
    const std::vector<BYTE>& image, VolumeOptions options, std::size_t dirs,
    std::uint64_t bytes, Tuning::MediaKind kind);
template Run ListWithDefaultTuning<Cache::Strategy::NoCache>(
    const std::vector<BYTE>& image, const VolumeOptions& options);
template Run ListWithDefaultTuning<Cache::Strategy::FullCache>(
    const std::vector<BYTE>& image, const VolumeOptions& options);
template LaneRun ListForLane<Cache::Strategy::NoCache>(
    const std::vector<BYTE>& image, const VolumeOptions& options,
    Tuning::MediaKind kind, bool raw_callback);
template LaneRun ListForLane<Cache::Strategy::FullCache>(
    const std::vector<BYTE>& image, const VolumeOptions& options,
    Tuning::MediaKind kind, bool raw_callback);
template TimedRun ListOnSimulatedDisk<Cache::Strategy::NoCache>(
    const std::vector<BYTE>& image, VolumeOptions options, std::size_t dirs,
    std::uint64_t bytes, const SimulatedDevice& device);
template TimedRun ListOnSimulatedDisk<Cache::Strategy::FullCache>(
    const std::vector<BYTE>& image, VolumeOptions options, std::size_t dirs,
    std::uint64_t bytes, const SimulatedDevice& device);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::ReadAhead
