#include "reader-support.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "scan-support.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Search {

namespace {

namespace Fake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;

// Clusters that ContentImage() takes at the end of the volume: three for
// data.bin, two for the real clusters of holes.bin and one for gone.bin.
constexpr std::uint64_t content_tail_clusters = 6;

// Writes ContentByte() of the file offset into the cluster at lcn, for the
// bytes of the cluster that the file holds from file_offset on.
void FillCluster(std::vector<BYTE>& image, std::uint64_t lcn,
                 std::uint64_t file_offset, std::uint64_t bytes) {
  const std::uint64_t base = lcn * Fake::fake_cluster_size;
  for (std::uint64_t i = 0; i < bytes; ++i) {
    image.at(static_cast<std::size_t>(base + i)) = ContentByte(file_offset + i);
  }
}

}  // namespace

std::vector<std::uint8_t> ContentBytes(std::uint64_t offset,
                                       std::uint64_t count) {
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    bytes.at(static_cast<std::size_t>(i)) = ContentByte(offset + i);
  }
  return bytes;
}

std::vector<BYTE> ContentImage() {
  using Fake::FakeDataRun;
  using Fake::FakeTreeNode;
  const auto build = [](std::uint64_t first) {
    std::vector<FakeTreeNode> nodes;
    nodes.push_back({.links = {{.name = L"data.bin"}},
                     .runs = {{.lcn = first, .clusters = 3}},
                     .stream_size = data_bin_size});
    nodes.push_back({.links = {{.name = L"holes.bin"}},
                     .runs = {{.lcn = first + 3, .clusters = 1},
                              {.lcn = std::nullopt, .clusters = 2},
                              {.lcn = first + 4, .clusters = 1}},
                     .stream_size = holes_bin_size});
    nodes.push_back({.links = {{.name = L"gone.bin"}},
                     .in_use = false,
                     .keep_entries = true,
                     .runs = {{.lcn = first + 5, .clusters = 1}},
                     .stream_size = gone_bin_size});
    return Fake::BuildFakeNtfsImageWithNameTree(nodes);
  };
  // The geometry does not depend on the runs: a first image tells where the
  // volume ends, and the runs go to its last clusters.
  const std::uint64_t clusters = build(0).size() / Fake::fake_cluster_size;
  const std::uint64_t first = clusters - content_tail_clusters;
  std::vector<BYTE> image = build(first);

  const std::uint64_t cluster = Fake::fake_cluster_size;
  FillCluster(image, first, 0, cluster);
  FillCluster(image, first + 1, cluster, cluster);
  FillCluster(image, first + 2, 2 * cluster, cluster - 100);
  FillCluster(image, first + 3, 0, cluster);
  FillCluster(image, first + 4, 3 * cluster, cluster);
  FillCluster(image, first + 5, 0, cluster);
  return image;
}

std::vector<Row> ListImage(const TempImage& image) {
  Lib::Options options;
  options.target = image.Target();
  options.engine = Lib::EngineKind::MftList;
  options.include_deleted = true;
  return RunScan(options).rows;
}

Row RowNamed(const std::vector<Row>& rows, std::u16string_view name) {
  for (const Row& row : rows) {
    if (row.name == name) {
      return row;
    }
  }
  return {};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Search
