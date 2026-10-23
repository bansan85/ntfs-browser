#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "nonresident-index-support.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// First byte of cluster index of the fixture's $ATTRIBUTE_LIST.
constexpr ULONGLONG ListCluster(DWORD index) {
  return static_cast<ULONGLONG>(Tests::multi_cluster_list_lcn + index) *
         Tests::fake_cluster_size;
}

// The listed record of the fixture, not attribute-parsed yet, and the reader
// that counts the reads of its volume.
template <Cache::Strategy S>
struct CountedListRecord {
  ParsedRecord<S> opened;
  Tests::CountingDiskReader* reader;
};

template <Cache::Strategy S>
CountedListRecord<S> OpenCountedListRecord(const VolumeOptions& options = {}) {
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(
          Tests::BuildFakeNtfsImageWithMultiClusterAttributeList()));
  Tests::CountingDiskReader* const counting = reader.get();
  ParsedRecord<S> opened = OpenRecordFrom<S>(
      std::move(reader), Tests::attr_list_lifetime_base_idx, options);
  // Only ParseAttrs() reads the mask.
  opened.record->SetAttrMask(Attr::Mask::Data);
  return {std::move(opened), counting};
}

// The bytes of the one $DATA record imported, or nothing when it has none.
template <Cache::Strategy S>
std::optional<std::vector<BYTE>> ImportedData(const Io::FileRecord<S>& record) {
  const auto& attrs = record.GetAttr(Attr::Type::Data);
  if (attrs.size() != 1) {
    return std::nullopt;
  }
  std::vector<BYTE> bytes(Tests::attr_list_lifetime_data_content.size());
  const std::optional<ULONGLONG> read = attrs.front()->ReadData(0, bytes);
  if (!read || *read != bytes.size()) {
    return std::nullopt;
  }
  return bytes;
}

// What the extension record's $DATA holds.
std::vector<BYTE> ExpectedData() {
  return {Tests::attr_list_lifetime_data_content.begin(),
          Tests::attr_list_lifetime_data_content.end()};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "a non-resident $ATTRIBUTE_LIST over two clusters imports what it names",
    "[attr][attribute-list]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const CountedListRecord<S> list = OpenCountedListRecord<S>();
  REQUIRE(list.opened.record->ParseAttrs());
  CHECK(ImportedData(*list.opened.record) == ExpectedData());
}

TEST_CASE("a non-resident $ATTRIBUTE_LIST reads each of its clusters once",
          "[attr][attribute-list]") {
  const CountedListRecord<Cache::Strategy::NoCache> list =
      OpenCountedListRecord<Cache::Strategy::NoCache>();
  list.reader->Reset();
  REQUIRE(list.opened.record->ParseAttrs());

  // 32 entries sit in the first cluster, 8 in the second.
  CHECK(list.reader->ReadsInRange(ListCluster(0), ListCluster(1)) == 1);
  CHECK(list.reader->ReadsInRange(ListCluster(1), ListCluster(2)) == 1);
}

TEST_CASE(
    "a non-resident $ATTRIBUTE_LIST whose second cluster cannot be read ends "
    "there",
    "[attr][attribute-list]") {
  const bool recover = GENERATE(false, true);
  const CountedListRecord<Cache::Strategy::NoCache> list =
      OpenCountedListRecord<Cache::Strategy::NoCache>(
          VolumeOptions{.recover_errors = recover});
  list.reader->FailReadsIn(ListCluster(1), ListCluster(2));
  list.reader->Reset();
  (void)Tests::TakeCapturedLog();

  const bool parsed = list.opened.record->ParseAttrs();
  CHECK_THAT(Tests::TakeCapturedLog(),
             ContainsSubstring("Attribute List ended at offset 1024 instead of "
                               "its declared size 1280."));
  // The walk stops at the failed cluster: it is never asked for again.
  CHECK(list.reader->ReadsInRange(ListCluster(1), ListCluster(2)) == 1);
  if (recover) {
    // The entry of the first cluster is kept.
    CHECK(list.opened.record->IsSalvaged());
    CHECK(ImportedData(*list.opened.record) == ExpectedData());
  } else {
    CHECK_FALSE(parsed);
    CHECK(list.opened.record->GetAttr(Attr::Type::Data).empty());
  }
}
