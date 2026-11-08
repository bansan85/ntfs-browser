#include "scan-support.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Search {

namespace {

namespace Fake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;

// Polls that RunScan() makes at most: a scan that polls this often without
// ending is stuck.
constexpr std::size_t max_polls = 100000;

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
          .has_clusters = entry.clusters != nullptr};
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
  for (std::size_t polls = 0; !scan.Finished() && polls < max_polls; ++polls) {
    scan.Poll();
  }
  outcome.state = scan.GetState();
  return outcome;
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
  nodes.push_back({.links = link(L"c.txt", 3), .index_size = 5});
  nodes.push_back({.links = link(L"Sub", 0), .directory = true});
  nodes.push_back(
      {.links = link(L"root.txt", Fake::name_tree_root), .index_size = 7});
  nodes.push_back({.links = link(L"gone.txt", 0),
                   .in_use = false,
                   .keep_entries = true,
                   .data_size = 3});
  return Fake::BuildFakeNtfsImageWithNameTree(nodes);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Search
