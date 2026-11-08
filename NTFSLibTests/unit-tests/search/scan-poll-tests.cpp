#include <algorithm>
#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>

#include "scan-support.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;

namespace {

// Options for the whole tree of TreeImage().
Lib::Options TreeOptions(const TestSearch::TempImage& image) {
  Lib::Options options;
  options.target = image.Target();
  options.recursive = true;
  return options;
}

}  // namespace

TEST_CASE("A scan that was not started does nothing", "[search][poll]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  Lib::Scan scan(TreeOptions(image));
  std::size_t calls = 0;
  scan.OnBatch([&calls](const Lib::Batch&) { ++calls; });

  CHECK(scan.GetState() == Lib::State::NotStarted);
  CHECK(scan.Poll() == Lib::State::NotStarted);
  CHECK_FALSE(scan.Finished());
  CHECK(calls == 0);
}

TEST_CASE("Poll delivers the entries and then the last batch",
          "[search][poll]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  Lib::Scan scan(TreeOptions(image));
  std::vector<bool> last;
  std::size_t entries = 0;
  scan.OnBatch([&](const Lib::Batch& batch) {
    last.push_back(batch.last);
    entries += batch.entries.size();
  });

  scan.Start();
  CHECK(scan.GetState() == Lib::State::Running);
  CHECK_FALSE(scan.Finished());
  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);

  CHECK(scan.Finished());
  CHECK(entries == 5);
  REQUIRE_FALSE(last.empty());
  CHECK(last.back());
  CHECK(std::count(last.begin(), last.end(), true) == 1);
  CHECK(scan.GetProgress().entries_found == 5);
}

TEST_CASE("Poll after the end calls nothing", "[search][poll]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  Lib::Scan scan(TreeOptions(image));
  std::size_t calls = 0;
  scan.OnBatch([&calls](const Lib::Batch&) { ++calls; });
  scan.Start();
  static_cast<void>(TestSearch::PollToEnd(scan));
  const std::size_t calls_at_end = calls;

  CHECK(scan.Poll() == Lib::State::Finished);
  CHECK(calls == calls_at_end);
}

TEST_CASE("Start twice scans once", "[search][poll]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  Lib::Scan scan(TreeOptions(image));
  std::size_t entries = 0;
  scan.OnBatch(
      [&entries](const Lib::Batch& batch) { entries += batch.entries.size(); });

  scan.Start();
  scan.Start();
  static_cast<void>(TestSearch::PollToEnd(scan));

  CHECK(entries == 5);
}

TEST_CASE("Cancel ends the scan as Cancelled", "[search][poll]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  Lib::Scan scan(TreeOptions(image));
  std::size_t last_batches = 0;
  scan.OnBatch([&last_batches](const Lib::Batch& batch) {
    if (batch.last) {
      ++last_batches;
    }
  });

  scan.Start();
  scan.Cancel();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Cancelled);
  CHECK(scan.Finished());
  CHECK(last_batches == 1);
}

TEST_CASE("A scan with no callback still ends", "[search][poll]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  Lib::Scan scan(TreeOptions(image));

  scan.Start();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
}

TEST_CASE("A scan destroyed before the end is harmless", "[search][poll]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  Lib::Scan scan(TreeOptions(image));
  scan.Start();
}
