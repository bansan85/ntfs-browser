#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "data/file-record-header.h"
#include "data/header-resident.h"
#include "data/standard-information.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::Data::StdInfoPermission;

namespace {

// $MFT starts at cluster 1 in every fake image, so record 0 sits one cluster
// in.
inline constexpr size_t fake_mft_offset = NtfsBrowserTests::fake_cluster_size;

// Root directory: MFT record 5 in every fake image.
inline constexpr ULONGLONG root_dir_idx = 5;

// Replaces the DOS permission bits of record idx's resident
// $STANDARD_INFORMATION.
void SetStdInfoPermission(std::vector<BYTE>& image, ULONGLONG idx,
                          StdInfoPermission permission) {
  const size_t record_offset =
      fake_mft_offset + idx * NtfsBrowserTests::fake_file_record_size;

  NtfsBrowser::Data::FileRecordHeader header{};
  std::memcpy(&header, &image[record_offset],
              NtfsBrowser::Data::FileRecordHeader::min_file_record_header_size);
  REQUIRE(header.magic ==
          NtfsBrowser::Data::FileRecordHeader::file_record_magic);

  // The first attribute of the fixture record is its $STANDARD_INFORMATION.
  const size_t attr_offset = record_offset + header.offset_of_attr;
  const auto& attr =
      *reinterpret_cast<const NtfsBrowser::Data::HeaderResident*>(
          &image[attr_offset]);
  REQUIRE(attr.header.type == Attr::Type::StandardInformation);

  const size_t permission_offset =
      attr_offset + attr.attr_offset +
      offsetof(NtfsBrowser::Data::StandardInformation, permission);
  const auto bits = static_cast<DWORD>(permission);
  std::memcpy(&image[permission_offset], &bits, sizeof(bits));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileRecord reports each STANDARD_INFORMATION flag accessor from its own "
    "permission bit",
    "[cov-frb]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  using Getter = bool (FileRecord<S>::*)() const noexcept;
  const std::array<std::pair<Getter, StdInfoPermission>, 9> flags{{
      {&FileRecord<S>::IsDevice, StdInfoPermission::Device},
      {&FileRecord<S>::IsNormal, StdInfoPermission::Normal},
      {&FileRecord<S>::IsTemporary, StdInfoPermission::Temp},
      {&FileRecord<S>::IsCompressed, StdInfoPermission::Compressed},
      {&FileRecord<S>::IsOffline, StdInfoPermission::Offline},
      {&FileRecord<S>::IsNotContentIndexed, StdInfoPermission::Nci},
      {&FileRecord<S>::IsEncrypted, StdInfoPermission::Encrypted},
      {&FileRecord<S>::IsSparse, StdInfoPermission::Sparse},
      {&FileRecord<S>::IsReparsePoint, StdInfoPermission::Reparse},
  }};

  for (const auto& [getter, bit] : flags) {
    std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImageWithMftTree();
    SetStdInfoPermission(image, NtfsBrowserTests::mft_tree_report_idx, bit);

    auto reader =
        std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image));
    const NtfsVolume<S> volume(std::move(reader));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(NtfsBrowserTests::mft_tree_report_idx));
    REQUIRE(record.ParseAttrs());
    CHECK((record.*getter)());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord's STANDARD_INFORMATION flag accessors return false when the "
    "record has no STANDARD_INFORMATION",
    "[cov-frb]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  REQUIRE(root.ParseFileRecord(root_dir_idx));
  REQUIRE(root.GetAttr(Attr::Type::StandardInformation).empty());

  CHECK_FALSE(root.IsDevice());
  CHECK_FALSE(root.IsNormal());
  CHECK_FALSE(root.IsTemporary());
  CHECK_FALSE(root.IsCompressed());
  CHECK_FALSE(root.IsOffline());
  CHECK_FALSE(root.IsNotContentIndexed());
  CHECK_FALSE(root.IsEncrypted());
  CHECK_FALSE(root.IsSparse());
  CHECK_FALSE(root.IsReparsePoint());
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord returns the Win32 name of a record that also has a DOS alias",
    "[cov-frb]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftTree());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::mft_tree_report_idx));
  REQUIRE(record.ParseAttrs());

  CHECK(record.GetFileName() == L"report.txt");
}
