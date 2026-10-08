#include "nps-ntfs1-test-support.h"

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/strategy.h>

#include "optional-access.h"

namespace NtfsBrowser {

class IDiskReader;
template <Strategy S>
class NtfsVolume;

}  // namespace NtfsBrowser

namespace NtfsBrowserTests {

using NtfsBrowser::AttrBase;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace {

// All names in known_files and this corpus's directories are ASCII, so a
// byte-for-byte widening is exact.
std::wstring Widen(std::string_view narrow) {
  return {narrow.begin(), narrow.end()};
}

void ParseDir(FileRecord<Strategy::NoCache>& dir, ULONGLONG file_ref) {
  dir.SetAttrMask(Mask::IndexRoot | Mask::IndexAllocation);
  REQUIRE(dir.ParseFileRecord(file_ref));
  REQUIRE(dir.ParseAttrs());
}

}  // namespace

std::unique_ptr<NtfsBrowser::IDiskReader> OpenNtfs1Image() {
  return OpenBareVolumeImage(ntfs1_image);
}

void OpenRootDir(FileRecord<Strategy::NoCache>& dir) {
  ParseDir(dir, static_cast<ULONGLONG>(MftIdx::Root));
}

void OpenSubDir(FileRecord<Strategy::NoCache>& dir, std::string_view name) {
  const std::optional<IndexEntry> entry = dir.FindSubEntry(Widen(name));
  REQUIRE(entry.has_value());
  ParseDir(dir, NtfsBrowserTests::Unwrap(entry).GetFileReference());
}

void OpenFile(FileRecord<Strategy::NoCache>& file,
              const FileRecord<Strategy::NoCache>& dir, std::string_view name) {
  const std::optional<IndexEntry> entry = dir.FindSubEntry(Widen(name));
  REQUIRE(entry.has_value());

  file.SetAttrMask(Mask::Data);
  REQUIRE(
      file.ParseFileRecord(NtfsBrowserTests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());
}

std::vector<BYTE> ReadFile(const NtfsVolume<Strategy::NoCache>& volume,
                           const FileRecord<Strategy::NoCache>& dir,
                           std::string_view name) {
  FileRecord<Strategy::NoCache> file(volume);
  OpenFile(file, dir, name);

  const AttrBase<Strategy::NoCache>* stream = file.FindStream({});
  REQUIRE(stream != nullptr);

  std::vector<BYTE> data(stream->GetDataSize());
  REQUIRE(stream->ReadData(0, data) == data.size());
  return data;
}

}  // namespace NtfsBrowserTests
