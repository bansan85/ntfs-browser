#include <ntfs-browser/win-types.h>

#include <array>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "named-stream-probe.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

template <Cache::Strategy S>
class AttrBase;

}  // namespace Attr

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {}  // namespace NTFS_BROWSER_NAMESPACE

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Attr::AttrBase;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

template <Cache::Strategy S>
void CheckFindStreamReturnsNamedStream() {
  auto reader = std::make_unique<
      NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNamedDataStream());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(record.ParseAttrs());

  // The "not found" case must still correctly return nullptr: no stream
  // (named or otherwise) in this fixture is named "nonexistent".
  CHECK(record.FindStream(L"nonexistent") == nullptr);

  const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<S>* stream =
      record.FindStream(NTFS_BROWSER_NAMESPACE::Tests::named_data_stream_name);
  REQUIRE(stream != nullptr);

  CHECK(stream->GetAttrName() ==
        NTFS_BROWSER_NAMESPACE::Tests::named_data_stream_name);

  // Confirm it's genuinely the named stream's own data, not some other
  // attribute.
  REQUIRE(stream->GetDataSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::named_data_stream_content.size());
  CHECK(std::memcmp(
            stream->GetData(),
            NTFS_BROWSER_NAMESPACE::Tests::named_data_stream_content.data(),
            NTFS_BROWSER_NAMESPACE::Tests::named_data_stream_content.size()) ==
        0);

  // Requesting the unnamed stream must still correctly return nullptr: this
  // fixture's only $DATA attribute is named, not unnamed.
  CHECK(record.FindStream({}) == nullptr);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("FindStream returns a named stream (ADS) by name",
                       "[file-record][regression]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckFindStreamReturnsNamedStream<S>();
}
