#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "alloc/types.h"
#include "attr/header-common.h"
#include "attr/mask.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "data/file-record-header.h"
#include "io/file-record.h"
#include "ntfs-volume.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Data = NTFS_BROWSER_NAMESPACE::Data;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;

namespace {

// Bytes of a list the tests ask the clusters of: more than any fixture holds.
constexpr ULONGLONG list_bytes_asked = ULONGLONG{256} * 1024;

// Byte offset of record idx in the fixture: its $MFT starts at cluster 1.
constexpr size_t RecordOffset(ULONGLONG idx) {
  return TestFakeImage::fake_cluster_size +
         (static_cast<size_t>(idx) * TestFakeImage::fake_file_record_size);
}

// The bytes of record idx in image.
std::span<const BYTE> RecordBytes(const std::vector<BYTE>& image,
                                  ULONGLONG idx) {
  return std::span<const BYTE>{image}.subspan(
      RecordOffset(idx), TestFakeImage::fake_file_record_size);
}

// Reads of reader that touch record idx.
size_t ReadsOfRecord(const TestDiskReaders::CountingDiskReader& reader,
                     ULONGLONG idx) {
  return reader.ReadsInRange(RecordOffset(idx),
                             RecordOffset(idx) +
                                 TestFakeImage::fake_file_record_size);
}

// A volume over image, and the reader that counts its reads.
template <Cache::Strategy S>
struct CountedVolume {
  std::unique_ptr<NtfsVolume<S>> volume;
  TestDiskReaders::CountingDiskReader* reader;
};

template <Cache::Strategy S>
CountedVolume<S> OpenCounted(std::vector<BYTE> image,
                             const VolumeOptions& options = {}) {
  auto reader = std::make_unique<TestDiskReaders::CountingDiskReader>(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(std::move(image)));
  TestDiskReaders::CountingDiskReader* const counting = reader.get();
  auto volume = std::make_unique<NtfsVolume<S>>(std::move(reader), options);
  REQUIRE(volume->IsVolumeOK());
  return {std::move(volume), counting};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::HasAttrHeader() finds an attribute by its header alone",
    "[file-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithMftTreeAttributeList();

  SECTION("a parsed record") {
    const CountedVolume<S> opened = OpenCounted<S>(std::move(image));
    Io::FileRecord<S> record(*opened.volume);
    CHECK_FALSE(record.HasAttrHeader(Attr::Type::AttributeList));
    REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_list_base_idx));

    CHECK(record.HasAttrHeader(Attr::Type::StandardInformation));
    CHECK(record.HasAttrHeader(Attr::Type::AttributeList));
    // The names and the data sit in the extension records.
    CHECK_FALSE(record.HasAttrHeader(Attr::Type::FileName));
    CHECK_FALSE(record.HasAttrHeader(Attr::Type::Data));
  }

  SECTION("a header of no length ends the walk") {
    const size_t record_offset =
        RecordOffset(TestFakeImage::mft_tree_list_base_idx);
    Data::FileRecordHeader header{};
    std::memcpy(&header, &image.at(record_offset), sizeof(header));
    // The first attribute, $STANDARD_INFORMATION, ahead of the list.
    const size_t total_size_offset = record_offset + header.offset_of_attr +
                                     offsetof(Attr::HeaderCommon, total_size);
    std::memset(&image.at(total_size_offset), 0,
                sizeof(Attr::HeaderCommon::total_size));

    const CountedVolume<S> opened = OpenCounted<S>(std::move(image));
    Io::FileRecord<S> record(*opened.volume);
    REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_list_base_idx));
    CHECK_FALSE(record.HasAttrHeader(Attr::Type::AttributeList));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::AttributeListExtents() maps a non-resident list by its "
    "header alone",
    "[file-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("a list in one cluster") {
    const CountedVolume<S> opened = OpenCounted<S>(
        TestFakeImage::BuildFakeNtfsImageWithMftTreeAttributeList(
            TestFakeImage::FakeListedExtension::Genuine,
            TestFakeImage::FakeListResidency::NonResident));
    Io::FileRecord<S> record(*opened.volume);
    CHECK(record.AttributeListExtents(list_bytes_asked).empty());
    REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_list_base_idx));
    opened.reader->Reset();

    const std::vector<Alloc::Extent> extents =
        record.AttributeListExtents(list_bytes_asked);
    REQUIRE(extents.size() == 1);
    CHECK(extents.front().lcn == TestFakeImage::mft_tree_list_lcn);
    CHECK(extents.front().clusters == 1);
    CHECK(opened.reader->Calls().empty());
    // The list the extents map is the one ParseAttrs() reads.
    record.SetParseOptions({.attr_mask = Attr::Mask::FileName});
    REQUIRE(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::FileName).size() == 2);
  }

  SECTION("a list over two clusters, asked for in part") {
    const CountedVolume<S> opened = OpenCounted<S>(
        TestFakeImage::BuildFakeNtfsImageWithMultiClusterAttributeList());
    Io::FileRecord<S> record(*opened.volume);
    REQUIRE(record.ParseFileRecord(TestFakeImage::attr_list_lifetime_base_idx));

    const auto [bytes, clusters] = GENERATE(
        table<ULONGLONG, ULONGLONG>({{1, 1},
                                     {TestFakeImage::fake_cluster_size, 1},
                                     {TestFakeImage::fake_cluster_size + 1, 2},
                                     {list_bytes_asked, 2}}));
    INFO("bytes asked: " << bytes);
    const std::vector<Alloc::Extent> extents =
        record.AttributeListExtents(bytes);
    REQUIRE(extents.size() == 1);
    CHECK(extents.front().lcn == TestFakeImage::multi_cluster_list_lcn);
    CHECK(extents.front().clusters == clusters);
    CHECK(record.AttributeListExtents(0).empty());
  }

  SECTION("a resident list, or none") {
    const CountedVolume<S> opened = OpenCounted<S>(
        TestFakeImage::BuildFakeNtfsImageWithMftTreeAttributeList());
    Io::FileRecord<S> record(*opened.volume);
    REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_list_base_idx));
    CHECK(record.AttributeListExtents(list_bytes_asked).empty());
    REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_docs_idx));
    CHECK(record.AttributeListExtents(list_bytes_asked).empty());
  }
}

TEST_CASE("RecordParseOptions::extension_source serves extension records",
          "[file-record]") {
  const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithMftTreeAttributeList();
  const CountedVolume<Cache::Strategy::NoCache> opened =
      OpenCounted<Cache::Strategy::NoCache>(image);
  Io::FileRecord<Cache::Strategy::NoCache> record(*opened.volume);
  Io::RecordParseOptions options{.attr_mask =
                                     Attr::Mask::FileName | Attr::Mask::Data};

  SECTION("a record the source gives is not read") {
    options.extension_source = [&image](ULONGLONG idx) {
      return idx == TestFakeImage::mft_tree_list_low_ext_idx ||
                     idx == TestFakeImage::mft_tree_list_high_ext_idx
                 ? RecordBytes(image, idx)
                 : std::span<const BYTE>{};
    };
    record.SetParseOptions(std::move(options));
    REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_list_base_idx));
    opened.reader->Reset();
    REQUIRE(record.ParseAttrs());

    CHECK(record.GetAttr(Attr::Type::FileName).size() == 2);
    CHECK(record.GetAttr(Attr::Type::Data).size() == 1);
    CHECK(ReadsOfRecord(*opened.reader,
                        TestFakeImage::mft_tree_list_low_ext_idx) == 0);
    CHECK(ReadsOfRecord(*opened.reader,
                        TestFakeImage::mft_tree_list_high_ext_idx) == 0);
  }

  SECTION("a record the source does not give is read, once per list entry") {
    options.extension_source = [](ULONGLONG /*idx*/) {
      return std::span<const BYTE>{};
    };
    record.SetParseOptions(std::move(options));
    REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_list_base_idx));
    opened.reader->Reset();
    REQUIRE(record.ParseAttrs());

    CHECK(record.GetAttr(Attr::Type::FileName).size() == 2);
    CHECK(ReadsOfRecord(*opened.reader,
                        TestFakeImage::mft_tree_list_low_ext_idx) == 1);
    CHECK(ReadsOfRecord(*opened.reader,
                        TestFakeImage::mft_tree_list_high_ext_idx) == 2);
  }
}

TEST_CASE("RecordParseOptions::base_source spares the read of a foreign slot",
          "[file-record]") {
  // The hard-linked file, which the Foreign extension names as its base.
  static constexpr ULONGLONG other_base = TestFakeImage::mft_tree_hard_link_idx;
  const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithMftTreeAttributeList(
          TestFakeImage::FakeListedExtension::Foreign);
  const bool recover = GENERATE(false, true);
  const CountedVolume<Cache::Strategy::NoCache> opened =
      OpenCounted<Cache::Strategy::NoCache>(
          image, VolumeOptions{.recover_errors = recover});
  Io::FileRecord<Cache::Strategy::NoCache> record(*opened.volume);
  Io::RecordParseOptions options{.attr_mask =
                                     Attr::Mask::FileName | Attr::Mask::Data,
                                 .extension_source = [](ULONGLONG /*idx*/) {
                                   return std::span<const BYTE>{};
                                 }};

  SECTION("a slot that names another base is not read") {
    options.base_source = [](ULONGLONG idx) -> std::optional<ULONGLONG> {
      if (idx == TestFakeImage::mft_tree_list_low_ext_idx) {
        return TestFakeImage::mft_tree_list_base_idx;
      }
      return other_base;
    };
    record.SetParseOptions(std::move(options));
    REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_list_base_idx));
    opened.reader->Reset();

    // The list names a record of another file: strict mode rejects it.
    CHECK(record.ParseAttrs() == recover);
    CHECK(ReadsOfRecord(*opened.reader,
                        TestFakeImage::mft_tree_list_high_ext_idx) == 0);
    CHECK(ReadsOfRecord(*opened.reader,
                        TestFakeImage::mft_tree_list_low_ext_idx) == 1);
    if (recover) {
      CHECK(record.GetAttr(Attr::Type::FileName).size() == 1);
    }
  }

  SECTION("a slot that names this record, or none, is read") {
    const bool known = GENERATE(false, true);
    options.base_source = [known](ULONGLONG /*idx*/) {
      return known ? std::optional<
                         ULONGLONG>{TestFakeImage::mft_tree_list_base_idx}
                   : std::nullopt;
    };
    record.SetParseOptions(std::move(options));
    REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_list_base_idx));
    opened.reader->Reset();

    CHECK(record.ParseAttrs() == recover);
    // Read as without a source, and found foreign by its own header.
    CHECK(ReadsOfRecord(*opened.reader,
                        TestFakeImage::mft_tree_list_high_ext_idx) ==
          (recover ? 2 : 1));
  }
}
