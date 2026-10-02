#include <ntfs-browser/win-types.h>

#include <array>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "named-stream-probe.h"

namespace NtfsBrowser
{
template <Strategy S>
class AttrBase;
}  // namespace NtfsBrowser

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

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  // The "not found" case must still correctly return nullptr: no stream
  // (named or otherwise) in this fixture is named "nonexistent".
  CHECK(record.FindStream(L"nonexistent") == nullptr);

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

TEMPLATE_TEST_CASE_SIG("FindStream returns a named stream (ADS) by name",
                       "[file-record][regression]", ((Strategy S), S),
                       Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  CheckFindStreamReturnsNamedStream<S>();
}
