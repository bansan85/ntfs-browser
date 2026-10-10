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

#include "optional-access.h"

namespace NtfsBrowser {

class IDiskReader;
template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NtfsBrowser

namespace NtfsBrowserTests {

using NtfsBrowser::IndexEntry;
using NtfsBrowser::Attr::AttrBase;
using NtfsBrowser::Io::FileRecord;
namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;

namespace {

// All names in known_files and this corpus's directories are ASCII, so a
// byte-for-byte widening is exact.
std::wstring Widen(std::string_view narrow) {
  return {narrow.begin(), narrow.end()};
}

void ParseDir(NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache>& dir,
              ULONGLONG file_ref) {
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(dir.ParseFileRecord(file_ref));
  REQUIRE(dir.ParseAttrs());
}

}  // namespace

std::unique_ptr<NtfsBrowser::IDiskReader> OpenNtfs1Image() {
  return OpenBareVolumeImage(ntfs1_image);
}

void OpenRootDir(NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache>& dir) {
  ParseDir(dir, static_cast<ULONGLONG>(Mft::Idx::Root));
}

void OpenSubDir(NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache>& dir,
                std::string_view name) {
  const std::optional<IndexEntry> entry = dir.FindSubEntry(Widen(name));
  REQUIRE(entry.has_value());
  ParseDir(dir, NtfsBrowserTests::Unwrap(entry).GetFileReference());
}

void OpenFile(NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache>& file,
              const NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache>& dir,
              std::string_view name) {
  const std::optional<IndexEntry> entry = dir.FindSubEntry(Widen(name));
  REQUIRE(entry.has_value());

  file.SetAttrMask(Attr::Mask::Data);
  REQUIRE(
      file.ParseFileRecord(NtfsBrowserTests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());
}

std::vector<BYTE>
    ReadFile(const NtfsVolume<Cache::Strategy::NoCache>& volume,
             const NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache>& dir,
             std::string_view name) {
  NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache> file(volume);
  OpenFile(file, dir, name);

  const NtfsBrowser::Attr::AttrBase<Cache::Strategy::NoCache>* stream =
      file.FindStream({});
  REQUIRE(stream != nullptr);

  std::vector<BYTE> data(stream->GetDataSize());
  REQUIRE(stream->ReadData(0, data) == data.size());
  return data;
}

}  // namespace NtfsBrowserTests
