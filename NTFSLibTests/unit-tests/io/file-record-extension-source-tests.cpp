#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "counting-disk-reader.h"
#include "data/file-record-header.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Data = NTFS_BROWSER_NAMESPACE::Data;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// Bytes of a list the tests ask the clusters of: more than any fixture holds.
constexpr ULONGLONG list_bytes_asked = ULONGLONG{256} * 1024;

// Byte offset of record idx in the fixture: its $MFT starts at cluster 1.
constexpr size_t RecordOffset(ULONGLONG idx) {
  return Tests::fake_cluster_size +
         (static_cast<size_t>(idx) * Tests::fake_file_record_size);
}

// The bytes of record idx in image.
std::span<const BYTE> RecordBytes(const std::vector<BYTE>& image,
                                  ULONGLONG idx) {
  return std::span<const BYTE>{image}.subspan(RecordOffset(idx),
                                              Tests::fake_file_record_size);
}

// Reads of reader that touch record idx.
size_t ReadsOfRecord(const Tests::CountingDiskReader& reader, ULONGLONG idx) {
  return reader.ReadsInRange(RecordOffset(idx),
                             RecordOffset(idx) + Tests::fake_file_record_size);
}

// A volume over image, and the reader that counts its reads.
template <Cache::Strategy S>
struct CountedVolume {
  std::unique_ptr<NtfsVolume<S>> volume;
  Tests::CountingDiskReader* reader;
};

template <Cache::Strategy S>
CountedVolume<S> OpenCounted(std::vector<BYTE> image) {
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(std::move(image)));
  Tests::CountingDiskReader* const counting = reader.get();
  auto volume = std::make_unique<NtfsVolume<S>>(std::move(reader));
  REQUIRE(volume->IsVolumeOK());
  return {std::move(volume), counting};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::HasAttrHeader() finds an attribute by its header alone",
    "[file-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithMftTreeAttributeList();

  SECTION("a parsed record") {
    const CountedVolume<S> opened = OpenCounted<S>(std::move(image));
    Io::FileRecord<S> record(*opened.volume);
    CHECK_FALSE(record.HasAttrHeader(Attr::Type::AttributeList));
    REQUIRE(record.ParseFileRecord(Tests::mft_tree_list_base_idx));

    CHECK(record.HasAttrHeader(Attr::Type::StandardInformation));
    CHECK(record.HasAttrHeader(Attr::Type::AttributeList));
    // The names and the data sit in the extension records.
    CHECK_FALSE(record.HasAttrHeader(Attr::Type::FileName));
    CHECK_FALSE(record.HasAttrHeader(Attr::Type::Data));
  }

  SECTION("a header of no length ends the walk") {
    const size_t record_offset = RecordOffset(Tests::mft_tree_list_base_idx);
    Data::FileRecordHeader header{};
    std::memcpy(&header, &image.at(record_offset), sizeof(header));
    // The first attribute, $STANDARD_INFORMATION, ahead of the list.
    const size_t total_size_offset = record_offset + header.offset_of_attr +
                                     offsetof(Attr::HeaderCommon, total_size);
    std::memset(&image.at(total_size_offset), 0,
                sizeof(Attr::HeaderCommon::total_size));

    const CountedVolume<S> opened = OpenCounted<S>(std::move(image));
    Io::FileRecord<S> record(*opened.volume);
    REQUIRE(record.ParseFileRecord(Tests::mft_tree_list_base_idx));
    CHECK_FALSE(record.HasAttrHeader(Attr::Type::AttributeList));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::AttributeListExtents() maps a non-resident list by its "
    "header alone",
    "[file-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("a list in one cluster") {
    const CountedVolume<S> opened =
        OpenCounted<S>(Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
            Tests::FakeListedExtension::Genuine,
            Tests::FakeListResidency::NonResident));
    Io::FileRecord<S> record(*opened.volume);
    CHECK(record.AttributeListExtents(list_bytes_asked).empty());
    REQUIRE(record.ParseFileRecord(Tests::mft_tree_list_base_idx));
    opened.reader->Reset();

    const std::vector<Alloc::Extent> extents =
        record.AttributeListExtents(list_bytes_asked);
    REQUIRE(extents.size() == 1);
    CHECK(extents.front().lcn == Tests::mft_tree_list_lcn);
    CHECK(extents.front().clusters == 1);
    CHECK(opened.reader->Calls().empty());
    // The list the extents map is the one ParseAttrs() reads.
    record.SetAttrMask(Attr::Mask::FileName);
    REQUIRE(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::FileName).size() == 2);
  }

  SECTION("a list over two clusters, asked for in part") {
    const CountedVolume<S> opened = OpenCounted<S>(
        Tests::BuildFakeNtfsImageWithMultiClusterAttributeList());
    Io::FileRecord<S> record(*opened.volume);
    REQUIRE(record.ParseFileRecord(Tests::attr_list_lifetime_base_idx));

    const auto [bytes, clusters] =
        GENERATE(table<ULONGLONG, ULONGLONG>({{1, 1},
                                              {Tests::fake_cluster_size, 1},
                                              {Tests::fake_cluster_size + 1, 2},
                                              {list_bytes_asked, 2}}));
    INFO("bytes asked: " << bytes);
    const std::vector<Alloc::Extent> extents =
        record.AttributeListExtents(bytes);
    REQUIRE(extents.size() == 1);
    CHECK(extents.front().lcn == Tests::multi_cluster_list_lcn);
    CHECK(extents.front().clusters == clusters);
    CHECK(record.AttributeListExtents(0).empty());
  }

  SECTION("a resident list, or none") {
    const CountedVolume<S> opened =
        OpenCounted<S>(Tests::BuildFakeNtfsImageWithMftTreeAttributeList());
    Io::FileRecord<S> record(*opened.volume);
    REQUIRE(record.ParseFileRecord(Tests::mft_tree_list_base_idx));
    CHECK(record.AttributeListExtents(list_bytes_asked).empty());
    REQUIRE(record.ParseFileRecord(Tests::mft_tree_docs_idx));
    CHECK(record.AttributeListExtents(list_bytes_asked).empty());
  }
}

TEST_CASE("FileRecord::SetExtensionSource() serves extension records",
          "[file-record]") {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithMftTreeAttributeList();
  const CountedVolume<Cache::Strategy::NoCache> opened =
      OpenCounted<Cache::Strategy::NoCache>(image);
  Io::FileRecord<Cache::Strategy::NoCache> record(*opened.volume);
  record.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);

  SECTION("a record the source gives is not read") {
    record.SetExtensionSource([&image](ULONGLONG idx) {
      return idx == Tests::mft_tree_list_low_ext_idx ||
                     idx == Tests::mft_tree_list_high_ext_idx
                 ? RecordBytes(image, idx)
                 : std::span<const BYTE>{};
    });
    REQUIRE(record.ParseFileRecord(Tests::mft_tree_list_base_idx));
    opened.reader->Reset();
    REQUIRE(record.ParseAttrs());

    CHECK(record.GetAttr(Attr::Type::FileName).size() == 2);
    CHECK(record.GetAttr(Attr::Type::Data).size() == 1);
    CHECK(ReadsOfRecord(*opened.reader, Tests::mft_tree_list_low_ext_idx) == 0);
    CHECK(ReadsOfRecord(*opened.reader, Tests::mft_tree_list_high_ext_idx) ==
          0);
  }

  SECTION("a record the source does not give is read, once per list entry") {
    record.SetExtensionSource(
        [](ULONGLONG /*idx*/) { return std::span<const BYTE>{}; });
    REQUIRE(record.ParseFileRecord(Tests::mft_tree_list_base_idx));
    opened.reader->Reset();
    REQUIRE(record.ParseAttrs());

    CHECK(record.GetAttr(Attr::Type::FileName).size() == 2);
    CHECK(ReadsOfRecord(*opened.reader, Tests::mft_tree_list_low_ext_idx) == 1);
    CHECK(ReadsOfRecord(*opened.reader, Tests::mft_tree_list_high_ext_idx) ==
          2);
  }
}
