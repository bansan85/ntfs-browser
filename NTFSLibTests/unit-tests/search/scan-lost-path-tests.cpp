#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/types.h>

#include "reader-support.h"
#include "scan-support.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE;
using TestSearch::TempImage;

TEST_CASE(
    "The path of a file in a folder with no name starts at the lost folder",
    "[search][scan]") {
  std::vector<TestFake::FakeTreeNode> nodes;
  nodes.push_back({.links = {}, .directory = true});
  nodes.push_back(
      {.links = {{.name = L"child.txt", .parent = 0}}, .data_size = 4});
  const TempImage image(TestFake::BuildFakeNtfsImageWithNameTree(nodes));
  Lib::Options options;
  options.target = image.Target();
  options.engine = Lib::EngineKind::MftList;
  options.full_path = true;

  const auto outcome = TestSearch::RunScan(options);

  CHECK(outcome.state == Lib::State::Finished);
  const auto child = TestSearch::RowNamed(outcome.rows, u"child.txt");
  REQUIRE(child.name == u"child.txt");
  CHECK(child.path == u"\\<lost>\\child.txt");
}
