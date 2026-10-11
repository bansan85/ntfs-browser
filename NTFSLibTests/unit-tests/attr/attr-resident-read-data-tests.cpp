#include <ntfs-browser/win-types.h>

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;

namespace {

// Fills the read buffer so untouched bytes stay recognizable.
constexpr BYTE sentinel_byte = 0xCC;

// Bigger than small_resident_data_content, like a real caller's fixed buffer.
constexpr size_t buffer_size_value = 8;

template <Cache::Strategy S>
void CheckReadDataReturnsActualByteCount() {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSmallResidentData());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(record.ParseAttrs());

  const auto& data_attrs = record.GetAttr(Attr::Type::Data);
  REQUIRE(data_attrs.size() == 1);

  std::array<BYTE, buffer_size_value> buffer{};
  buffer.fill(sentinel_byte);

  // The REQUIRE above checks the size of dataAttrs.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const std::optional<ULONGLONG> result = data_attrs[0]->ReadData(0, buffer);

  REQUIRE(result.has_value());
  CHECK(NtfsBrowserTests::Unwrap(result) ==
        NtfsBrowserTests::small_resident_data_content.size());

  CHECK(std::memcmp(buffer.data(),
                    NtfsBrowserTests::small_resident_data_content.data(),
                    NtfsBrowserTests::small_resident_data_content.size()) == 0);

  // Bytes past the attribute's real size must remain untouched sentinels.
  for (size_t i = NtfsBrowserTests::small_resident_data_content.size();
       i < buffer.size(); i++) {
    // i < buffer.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(buffer[i] == sentinel_byte);
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "AttrResident::ReadData returns the actual bytes copied, not the "
    "requested buffer size",
    "[attr-resident][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckReadDataReturnsActualByteCount<S>();
}
