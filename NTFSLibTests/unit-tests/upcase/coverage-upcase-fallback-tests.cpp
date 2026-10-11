#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "volume-tree-support.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;
using NtfsBrowserTests::MemoryDiskReader;
using NtfsBrowserTests::NonAsciiNameLayout;

namespace {

// Offset of a non-resident header's real size.
constexpr size_t attr_real_size_field = 48;

// Offset of a non-resident header's initialized size.
constexpr size_t attr_ini_size_field = 56;

// Offset of a non-resident header's data run list, a WORD.
constexpr size_t attr_data_run_offset_field = 32;

// Size a $UpCase DATA attribute is shrunk to, far below the 128 KiB table.
constexpr ULONGLONG short_upcase_size = 100;

// Record number of $UpCase.
constexpr ULONGLONG upcase_idx = static_cast<ULONGLONG>(Mft::Idx::UpCase);

// First physical cluster of a non-resident attribute's first data run.
[[nodiscard]] ULONGLONG FirstRunLcn(const std::vector<BYTE>& image,
                                    size_t attr) {
  const size_t run =
      attr + Load<WORD>(image, attr + attr_data_run_offset_field);
  const auto header = Load<BYTE>(image, run);
  const size_t length_bytes = header & 0x0FU;
  const size_t lcn_bytes = header >> 4U;
  const size_t lcn_field = run + 1 + length_bytes;
  ULONGLONG lcn = 0;
  for (size_t i = 0; i < lcn_bytes; i++) {
    lcn |= static_cast<ULONGLONG>(Load<BYTE>(image, lcn_field + i)) << (8U * i);
  }
  return lcn;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry still finds a name when $UpCase is unusable", "[cov-vol]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithNonAsciiNames(
          NonAsciiNameLayout::IndexRoot, true);

  SECTION("$UpCase record does not parse") { ZeroRecord(image, upcase_idx); }

  SECTION("$UpCase data is shorter than the table") {
    Store<ULONGLONG>(image,
                     FirstAttr(image, upcase_idx, Attr::Type::Data) +
                         attr_real_size_field,
                     short_upcase_size);
  }

  SECTION("$UpCase initialized size is shorter than the table") {
    Store<ULONGLONG>(image,
                     FirstAttr(image, upcase_idx, Attr::Type::Data) +
                         attr_ini_size_field,
                     short_upcase_size);
  }

  SECTION("$UpCase table does not map a to A") {
    const size_t attr = FirstAttr(image, upcase_idx, Attr::Type::Data);
    const auto table_offset =
        FirstRunLcn(image, attr) * NtfsBrowserTests::fake_cluster_size;
    std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(table_offset),
                NtfsBrowserTests::fake_cluster_size, BYTE{0});
  }

  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(root_idx));
  REQUIRE(root.ParseAttrs());

  const std::optional<NtfsBrowser::IndexEntry> found =
      root.FindSubEntry(NtfsBrowserTests::non_ascii_acute_name);
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == NtfsBrowserTests::non_ascii_acute_mft_ref);
}

TEST_CASE("FindSubEntry falls back when a $UpCase read is refused",
          "[cov-vol]") {
  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithNonAsciiNames(
          NonAsciiNameLayout::IndexRoot, true);
  const ULONGLONG table_begin =
      FirstRunLcn(image, FirstAttr(image, upcase_idx, Attr::Type::Data)) *
      NtfsBrowserTests::fake_cluster_size;
  auto refused = std::make_shared<size_t>(0);
  const NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<RefusingDiskReader>(
          std::move(image), table_begin,
          table_begin + NtfsBrowserTests::fake_cluster_size, refused));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(root_idx));
  REQUIRE(root.ParseAttrs());

  const std::optional<NtfsBrowser::IndexEntry> found =
      root.FindSubEntry(NtfsBrowserTests::non_ascii_acute_name);
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == NtfsBrowserTests::non_ascii_acute_mft_ref);
  CHECK(*refused > 0);
}
