#include <array>
#include <cstring>
#include <memory>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// Fills the read buffer so untouched bytes stay recognizable.
constexpr BYTE kSentinelByte = 0xCC;

// Bigger than kSmallResidentDataContent, like a real caller's fixed buffer.
constexpr size_t kBufferSize = 8;

template <Strategy S>
void CheckReadDataReturnsActualByteCount()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSmallResidentData());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  const auto& dataAttrs = record.getAttr(AttrType::DATA);
  REQUIRE(dataAttrs.size() == 1);

  std::array<BYTE, kBufferSize> buffer{};
  buffer.fill(kSentinelByte);

  const std::optional<ULONGLONG> result = dataAttrs[0]->ReadData(0, buffer);

  REQUIRE(result.has_value());
  CHECK(*result == NtfsBrowserTests::kSmallResidentDataContent.size());

  CHECK(std::memcmp(buffer.data(),
                    NtfsBrowserTests::kSmallResidentDataContent.data(),
                    NtfsBrowserTests::kSmallResidentDataContent.size()) == 0);

  // Bytes past the attribute's real size must remain untouched sentinels.
  for (size_t i = NtfsBrowserTests::kSmallResidentDataContent.size();
       i < buffer.size(); i++)
  {
    CHECK(buffer[i] == kSentinelByte);
  }
}

}  // namespace

TEST_CASE(
    "AttrResident::ReadData returns the actual bytes copied, not the "
    "requested buffer size",
    "[attr-resident][regression]")
{
  CheckReadDataReturnsActualByteCount<Strategy::NO_CACHE>();
}

TEST_CASE(
    "AttrResident::ReadData returns the actual bytes copied, not the "
    "requested buffer size (FULL_CACHE)",
    "[attr-resident][regression]")
{
  CheckReadDataReturnsActualByteCount<Strategy::FULL_CACHE>();
}
