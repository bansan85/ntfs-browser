#include <cstring>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrBase;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

template <Strategy S>
void CheckFindStreamReturnsNamedStream()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithNamedDataStream());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  // The "not found" case must still correctly return nullptr: no stream
  // (named or otherwise) in this fixture is named "nonexistent".
  CHECK(record.FindStream(L"nonexistent") == nullptr);

  // Before the F18 fix, FindStream()'s named-stream branch does `break;`
  // instead of `return data.get();`, so it falls out of the loop into the
  // same `return nullptr;` used above for "not found" - a named stream can
  // never be returned even though one exists with a matching name.
  const AttrBase<S>* stream =
      record.FindStream(NtfsBrowserTests::kNamedDataStreamName);
  REQUIRE(stream != nullptr);

  CHECK(stream->GetAttrName() == NtfsBrowserTests::kNamedDataStreamName);

  // Confirm it's genuinely the named stream's own data, not some other
  // attribute.
  REQUIRE(stream->GetDataSize() ==
          NtfsBrowserTests::kNamedDataStreamContent.size());
  CHECK(std::memcmp(stream->GetData(),
                    NtfsBrowserTests::kNamedDataStreamContent.data(),
                    NtfsBrowserTests::kNamedDataStreamContent.size()) == 0);

  // Requesting the unnamed stream must still correctly return nullptr: this
  // fixture's only $DATA attribute is named, not unnamed.
  CHECK(record.FindStream({}) == nullptr);
}

}  // namespace

// Regression test for bug F18 (docs/bug-reports/2026-09-03-full-repo.md):
// FileRecord<S>::FindStream() (src/file-record.cpp)'s named-stream branch
// does `break;` instead of `return data.get();` - a copy-paste mistake that
// means asking for a named Alternate Data Stream (ADS) by name never
// actually returns it, even when one exists with a matching name.
TEST_CASE("FindStream returns a named stream (ADS) by name (F18)",
          "[file-record][regression]")
{
  CheckFindStreamReturnsNamedStream<Strategy::NO_CACHE>();
}

// Same fixture under FULL_CACHE: FindStream() (src/file-record.cpp) runs
// identically regardless of strategy, mirroring the paired NO_CACHE/
// FULL_CACHE tests elsewhere in this test suite (eg.
// attr-resident-read-data-tests.cpp).
TEST_CASE("FindStream returns a named stream (ADS) by name (F18, FULL_CACHE)",
          "[file-record][regression]")
{
  CheckFindStreamReturnsNamedStream<Strategy::FULL_CACHE>();
}
