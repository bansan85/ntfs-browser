#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

// Regression test for bug F2: AttrBase<S>::GetAttrName() (src/attr-base.cpp)
// builds a std::wstring_view straight off attr_header_.name_offset/
// name_length without ever checking name_offset + 2*name_length against
// total_size (or the record's own end). name_offset is a WORD (0..65535)
// and name_length a BYTE counted in UTF-16 code units (up to 510 bytes), so
// a forged attribute can make GetAttrName() return a view reaching up to
// ~64KiB + 510 bytes past a 1024-byte record buffer.
//
// BuildFakeNtfsImageWithAttrNameExceedsTotalSize() forges exactly that
// shape on a single resident $DATA attribute: total_size only covers the
// header plus a 4-byte body, but name_offset/name_length reach 112 bytes
// in - past total_size, but still comfortably inside the same 1024-byte
// record buffer, at a fixed offset this fixture itself writes a
// recognizable sentinel string into (kAttrNameBoundsSentinel). This keeps
// the wrong (pre-fix) result deterministic - GetAttrName() actually
// returning that sentinel string - instead of depending on whatever
// happens to be in adjacent heap memory, the way a "point past the 1024
// byte buffer entirely" fixture would (see the bug report's atteignabilité
// discussion, and parse-attrs-total-size-tests.cpp's F1 fixture for the
// same reasoning).
//
// Before the fix, GetAttrName() wrongly returns a non-empty view holding
// kAttrNameBoundsSentinel's bytes (the CHECK below fails). After the fix,
// name_offset + 2*name_length (112) > total_size (28) is caught and
// GetAttrName() returns {} instead.
TEST_CASE(
    "GetAttrName rejects a name whose offset/length exceed the attribute's "
    "total_size (F2)",
    "[attr-base][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttrNameExceedsTotalSize());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::kAttrNameExceedsTotalSizeRecordIdx));
  REQUIRE(record.ParseAttrs());

  const auto& dataAttrs = record.getAttr(AttrType::DATA);
  REQUIRE(dataAttrs.size() == 1);

  CHECK(dataAttrs[0]->GetAttrName().empty());
}

// Same fixture under FULL_CACHE: AttrBase<S>::GetAttrName() runs identically
// regardless of strategy (it only ever reads through attr_header_, a
// reference bound at construction time in both strategies) - confirms the
// defect (and its fix) isn't an artifact of one strategy, mirroring F1's
// two-strategy pattern in parse-attrs-total-size-tests.cpp.
TEST_CASE(
    "GetAttrName rejects a name whose offset/length exceed the attribute's "
    "total_size (F2, FULL_CACHE)",
    "[attr-base][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttrNameExceedsTotalSize());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::kAttrNameExceedsTotalSizeRecordIdx));
  REQUIRE(record.ParseAttrs());

  const auto& dataAttrs = record.getAttr(AttrType::DATA);
  REQUIRE(dataAttrs.size() == 1);

  CHECK(dataAttrs[0]->GetAttrName().empty());
}
