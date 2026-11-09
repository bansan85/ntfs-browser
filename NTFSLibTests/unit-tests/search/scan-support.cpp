#include "scan-support.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/types.h>

#include "utf/utf.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Search {

namespace {

namespace Fake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE;

// How long PollToEnd() waits for a scan: far more than any test scan needs, so
// that only a stuck scan reaches it.
constexpr std::chrono::seconds poll_deadline{60};

Row RowOf(const Lib::Entry& entry) {
  return {.name = std::u16string(entry.name),
          .path = std::u16string(entry.path),
          .record = entry.record,
          .sequence = entry.sequence,
          .parent_record = entry.parent_record,
          .size = entry.size,
          .allocated_size = entry.allocated_size,
          .created = entry.created.ticks,
          .modified = entry.modified.ticks,
          .mft_changed = entry.mft_changed.ticks,
          .accessed = entry.accessed.ticks,
          .attributes = entry.attributes,
          .is_directory = entry.is_directory,
          .standing = entry.standing,
          .link_count = entry.link_count,
          .data_state = entry.data_state,
          .has_clusters = entry.clusters != nullptr,
          .handle = entry.GetHandle()};
}

}  // namespace

TempImage::TempImage(const std::vector<BYTE>& image)
    : path_(Fake::WriteFakeNtfsImage(image)) {}

TempImage::~TempImage() {
  std::error_code ignored;
  std::filesystem::remove(path_, ignored);
}

std::u16string TempImage::Target(std::u16string_view inside) const {
  std::u16string target = path_.generic_u16string();
  if (!inside.empty()) {
    target += u'/';
    target += inside;
  }
  return target;
}

Outcome RunScan(const Lib::Options& options) {
  Outcome outcome;
  Lib::Scan scan(options);
  scan.OnBatch([&outcome](const Lib::Batch& batch) {
    ++outcome.batches;
    if (batch.last) {
      ++outcome.last_batches;
    }
    outcome.progress = batch.progress;
    for (const Lib::Entry& entry : batch.entries) {
      outcome.rows.push_back(RowOf(entry));
    }
  });
  scan.Start();
  PollToEnd(scan);
  outcome.state = scan.GetState();
  return outcome;
}

Lib::State PollToEnd(Lib::Scan& scan) {
  const auto deadline = std::chrono::steady_clock::now() + poll_deadline;
  while (!scan.Finished() && std::chrono::steady_clock::now() < deadline) {
    scan.Poll();
    std::this_thread::yield();
  }
  return scan.GetState();
}

std::vector<std::string> Names(const std::vector<Row>& rows) {
  std::vector<std::string> names;
  for (const Row& row : rows) {
    names.push_back(NTFS_BROWSER_NAMESPACE::Utf::WideToUtf8(
        NTFS_BROWSER_NAMESPACE::Utf::Utf16ToWide(row.name)));
  }
  return names;
}

std::vector<BYTE> TreeImage() {
  using Fake::FakeTreeLink;
  using Fake::FakeTreeNode;
  const auto link = [](std::wstring name, std::size_t parent) {
    return std::vector<FakeTreeLink>{
        {.name = std::move(name), .parent = parent}};
  };
  std::vector<FakeTreeNode> nodes;
  nodes.push_back(
      {.links = link(L"Docs", Fake::name_tree_root), .directory = true});
  nodes.push_back({.links = link(L"a.txt", 0),
                   .data_size = 10,
                   .index_size = 10,
                   .index_allocated_size = 4096});
  nodes.push_back(
      {.links = link(L"c.txt", 3), .data_size = 5, .index_size = 5});
  nodes.push_back({.links = link(L"Sub", 0), .directory = true});
  nodes.push_back({.links = link(L"root.txt", Fake::name_tree_root),
                   .data_size = 7,
                   .index_size = 7});
  nodes.push_back({.links = link(L"gone.txt", 0),
                   .in_use = false,
                   .keep_entries = true,
                   .data_size = 3});
  return Fake::BuildFakeNtfsImageWithNameTree(nodes);
}

std::vector<BYTE> HardLinkImage() {
  using Fake::FakeTreeNode;
  std::vector<FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"Docs"}}, .directory = true});
  nodes.push_back(
      {.links = {{.name = L"one.txt"}, {.name = L"two.txt", .parent = 0}},
       .data_size = 4});
  nodes.push_back({.links = {{.name = L"LongName.txt"},
                             {.name = L"LONGNA~1.TXT", .name_space = 2}},
                   .data_size = 2});
  return Fake::BuildFakeNtfsImageWithNameTree(nodes);
}

std::vector<BYTE> OrphanImage() {
  using Fake::FakeTreeNode;
  std::vector<FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"Keep"}}, .directory = true});
  nodes.push_back({.links = {{.name = L"k.txt", .parent = 0}}, .data_size = 1});
  nodes.push_back(
      {.links = {{.name = L"Gone"}}, .directory = true, .in_use = false});
  nodes.push_back(
      {.links = {{.name = L"Mid", .parent = 2}}, .directory = true});
  nodes.push_back(
      {.links = {{.name = L"orphan.txt", .parent = 3}}, .data_size = 1});
  return Fake::BuildFakeNtfsImageWithNameTree(nodes);
}

std::vector<BYTE> NamesImage() {
  using Fake::FakeTreeNode;
  // A name from UTF-16 units, as the volume stores it. It keeps a surrogate
  // that has no partner, which a wide literal could not on every host.
  const auto name = [](std::u16string_view units) {
    return NTFS_BROWSER_NAMESPACE::Utf::Utf16ToWide(units);
  };
  std::vector<FakeTreeNode> nodes;
  const auto file = [&nodes, &name](std::u16string_view units,
                                    std::size_t parent) {
    nodes.push_back(
        {.links = {{.name = name(units), .parent = parent}}, .data_size = 1});
  };
  nodes.push_back({.links = {{.name = L"Docs"}}, .directory = true});
  file(u"Caf\u00E9.txt", 0);
  file(u"\u0395\u03BB\u03BB\u03AC\u03B4\u03B1.txt", 0);
  file(u"\u041F\u0440\u0438\u0432\u0435\u0442.txt", 0);
  nodes.push_back({.links = {{.name = L"report.txt", .parent = 0},
                             {.name = L"Copy of report.txt"}},
                   .data_size = 1});
  file(u"CAF\u00C9.DOC", Fake::name_tree_root);
  file(u"\u0395\u039B\u039B\u0386\u0394\u0391.DOC", Fake::name_tree_root);
  file(u"\u041F\u0420\u0418\u0412\u0415\u0422.DOC", Fake::name_tree_root);
  file(u"a\U0001D11Eb.txt", Fake::name_tree_root);
  file(u"axb.txt", Fake::name_tree_root);
  file(
      u"a\xD834"
      u"b.txt",
      Fake::name_tree_root);
  nodes.push_back({.links = {{.name = name(u"gone-\u00E9.txt")}},
                   .in_use = false,
                   .keep_entries = true,
                   .data_size = 1});
  nodes.push_back({.links = {{.name = L"Deep"}}, .directory = true});
  nodes.push_back(
      {.links = {{.name = L"Deeper", .parent = 12}}, .directory = true});
  file(u"deep.txt", 13);
  return Fake::BuildFakeNtfsImageWithNameTree(nodes);
}

std::uint64_t FilterDay(std::uint32_t day) {
  // 2024-01-01 12:00:00 UTC, in seconds since 1970.
  constexpr std::int64_t first_day_seconds = 1'704'110'400;
  constexpr std::int64_t seconds_per_day = 86400;
  return Lib::Date::FromUnixSeconds(first_day_seconds + (day * seconds_per_day))
      .ticks;
}

std::vector<BYTE> FilterImage() {
  using Fake::FakeTreeNode;
  using Fake::FakeTreeTimes;
  const auto times = [](std::uint32_t created, std::uint32_t modified,
                        std::uint32_t mft_changed, std::uint32_t accessed) {
    return FakeTreeTimes{.created = FilterDay(created),
                         .modified = FilterDay(modified),
                         .mft_changed = FilterDay(mft_changed),
                         .accessed = FilterDay(accessed)};
  };
  const auto same = [&times](std::uint32_t day) {
    return times(day, day, day, day);
  };
  // A file of size bytes, allocated alloc, with the sizes of its index entries
  // equal to them.
  const auto file = [](std::wstring name, std::size_t parent, DWORD size,
                       ULONGLONG alloc, DWORD attributes, FakeTreeTimes when) {
    return FakeTreeNode{.links = {{.name = std::move(name), .parent = parent}},
                        .data_size = size,
                        .index_size = size,
                        .index_allocated_size = alloc,
                        .attributes = attributes,
                        .times = when};
  };
  // The nodes that others are filed under: Docs, and Sub in it.
  constexpr std::size_t docs = 0;
  constexpr std::size_t sub = 4;
  std::vector<FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"Docs"}},
                   .directory = true,
                   .attributes = Lib::Attribute::directory,
                   .times = same(10)});
  nodes.push_back(file(L"small.txt", Fake::name_tree_root, 8, 8,
                       Lib::Attribute::archive, times(10, 40, 40, 100)));
  nodes.push_back(file(L"medium.log", Fake::name_tree_root, 100, 104,
                       Lib::Attribute::archive | Lib::Attribute::hidden,
                       times(40, 100, 100, 150)));
  nodes.push_back(file(L"big.dat", docs, 400, 400, Lib::Attribute::read_only,
                       times(100, 150, 150, 200)));
  nodes.push_back({.links = {{.name = L"Sub", .parent = docs}},
                   .directory = true,
                   .attributes = Lib::Attribute::hidden,
                   .times = same(150)});
  nodes.push_back(file(L"deep.txt", sub, 100, 104,
                       Lib::Attribute::archive | Lib::Attribute::system,
                       same(200)));
  nodes.push_back(
      {.links = {{.name = L"link.txt"}, {.name = L"link2.txt", .parent = docs}},
       .data_size = 50,
       .index_size = 50,
       .index_allocated_size = 56,
       .times = same(250)});
  FakeTreeNode gone = file(L"gone.txt", Fake::name_tree_root, 20, 24,
                           Lib::Attribute::archive, same(40));
  gone.in_use = false;
  gone.keep_entries = true;
  nodes.push_back(std::move(gone));
  nodes.push_back(
      file(L"temp.bin", Fake::name_tree_root, 4, 8,
           Lib::Attribute::temporary | Lib::Attribute::reparse_point, same(5)));
  return Fake::BuildFakeNtfsImageWithNameTree(nodes);
}

std::vector<BYTE> ReallocatedImage() {
  using Fake::FakeDataRun;
  using Fake::FakeTreeNode;
  // The geometry does not depend on the runs: a first image tells where the
  // volume ends, and the runs go to its last clusters.
  const auto build = [](std::uint64_t first_lcn) {
    const auto stream = [first_lcn](std::uint64_t offset,
                                    std::uint32_t clusters) {
      return std::vector<FakeDataRun>{
          {.lcn = first_lcn + offset, .clusters = clusters}};
    };
    std::vector<FakeTreeNode> nodes;
    nodes.push_back({.links = {{.name = L"live.bin"}},
                     .runs = stream(0, 2),
                     .stream_size = 2ULL * Fake::fake_cluster_size});
    nodes.push_back({.links = {{.name = L"old.bin"}},
                     .in_use = false,
                     .keep_entries = true,
                     .runs = stream(0, 2),
                     .stream_size = 2ULL * Fake::fake_cluster_size});
    nodes.push_back({.links = {{.name = L"free.bin"}},
                     .in_use = false,
                     .keep_entries = true,
                     .runs = stream(2, 1),
                     .stream_size = Fake::fake_cluster_size});
    return Fake::BuildFakeNtfsImageWithNameTree(nodes);
  };
  const std::uint64_t clusters = build(0).size() / Fake::fake_cluster_size;
  return build(clusters - reallocated_tail_clusters);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Search
