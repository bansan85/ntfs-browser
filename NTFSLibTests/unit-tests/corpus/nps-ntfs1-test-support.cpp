#include "nps-ntfs1-test-support.h"

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "optional-access.h"

namespace NTFS_BROWSER_NAMESPACE {

class IDiskReader;
template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Tests {

using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::Attr::AttrBase;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

// All names in known_files and this corpus's directories are ASCII, so a
// byte-for-byte widening is exact.
std::wstring Widen(std::string_view narrow) {
  return {narrow.begin(), narrow.end()};
}

void ParseDir(
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>& dir,
    ULONGLONG file_ref) {
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(dir.ParseFileRecord(file_ref));
  REQUIRE(dir.ParseAttrs());
}

}  // namespace

std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> OpenNtfs1Image() {
  return OpenBareVolumeImage(ntfs1_image);
}

void OpenRootDir(
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>& dir) {
  ParseDir(dir, static_cast<ULONGLONG>(Mft::Idx::Root));
}

void OpenSubDir(
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>& dir,
    std::string_view name) {
  const std::optional<IndexEntry> entry = dir.FindSubEntry(Widen(name));
  REQUIRE(entry.has_value());
  ParseDir(dir,
           NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileReference());
}

void OpenFile(
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>& file,
    const NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>& dir,
    std::string_view name) {
  const std::optional<IndexEntry> entry = dir.FindSubEntry(Widen(name));
  REQUIRE(entry.has_value());

  file.SetAttrMask(Attr::Mask::Data);
  REQUIRE(file.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());
}

std::vector<BYTE> ReadFile(
    const NtfsVolume<Cache::Strategy::NoCache>& volume,
    const NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>& dir,
    std::string_view name) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> file(volume);
  OpenFile(file, dir, name);

  const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<Cache::Strategy::NoCache>*
      stream = file.FindStream({});
  REQUIRE(stream != nullptr);

  std::vector<BYTE> data(stream->GetDataSize());
  REQUIRE(stream->ReadData(0, data) == data.size());
  return data;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
