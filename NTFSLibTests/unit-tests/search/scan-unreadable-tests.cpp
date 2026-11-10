#include <algorithm>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/types.h>

#include "reader-support.h"
#include "scan-support.h"
#include "win-types.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE;
using TestSearch::RunScan;
using TestSearch::TempImage;

namespace {

// Record of Docs in TreeImage(), the first node of its tree.
const std::uint64_t docs_record = TestSearch::Record(0);

// TreeImage() with the record of Docs filled with zeros, so it does not parse.
std::vector<BYTE> ImageWithoutDocs() {
  std::vector<BYTE> image = TestSearch::TreeImage();
  const std::uint64_t at = TestSearch::RecordOffsetIn(image, docs_record);
  std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(at),
              TestFake::fake_file_record_size, BYTE{0});
  return image;
}

}  // namespace

TEST_CASE("A scan of a folder whose record does not parse fails",
          "[search][scan]") {
  const TempImage image(ImageWithoutDocs());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  const bool recover = GENERATE(false, true);
  Lib::Options options;
  options.target = image.Target(u"Docs");
  options.engine = engine;
  options.recover_errors = recover;

  const auto outcome = RunScan(options);

  CHECK(outcome.state == Lib::State::Failed);
  CHECK(outcome.rows.empty());
}

TEST_CASE("A scan of a volume whose root does not parse fails",
          "[search][scan]") {
  const TempImage image(TestFake::BuildFakeNtfsImageWithCorruptRootRecord());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options;
  options.target = image.Target(u"Docs");
  options.engine = engine;

  const auto outcome = RunScan(options);

  CHECK(outcome.state == Lib::State::Failed);
}

TEST_CASE("A recursive scan goes on past a folder that does not parse",
          "[search][scan]") {
  const TempImage image(ImageWithoutDocs());
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options;
  options.target = image.Target();
  options.engine = engine;
  options.recursive = true;
  options.recover_errors = true;

  const auto outcome = RunScan(options);

  CHECK(outcome.state == Lib::State::Finished);
  const auto root_txt = TestSearch::RowNamed(outcome.rows, u"root.txt");
  CHECK(root_txt.name == u"root.txt");
}
