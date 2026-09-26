#include "nps-ntfs1-test-support.h"

#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>

namespace NtfsBrowserTests
{

using NtfsBrowser::AttrBase;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// All names in kKnownFiles and this corpus's directories are ASCII, so a
// byte-for-byte widening is exact.
std::wstring Widen(std::string_view narrow)
{
  return {narrow.begin(), narrow.end()};
}

void ParseDir(FileRecord<Strategy::NO_CACHE>& dir, ULONGLONG fileRef)
{
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
  REQUIRE(dir.ParseFileRecord(fileRef));
  REQUIRE(dir.ParseAttrs());
}

}  // namespace

void OpenRootDir(FileRecord<Strategy::NO_CACHE>& dir)
{
  ParseDir(dir, static_cast<ULONGLONG>(MftIdx::ROOT));
}

void OpenSubDir(FileRecord<Strategy::NO_CACHE>& dir, std::string_view name)
{
  const std::optional<IndexEntry> entry = dir.FindSubEntry(Widen(name));
  REQUIRE(entry.has_value());
  ParseDir(dir, entry->GetFileReference());
}

void OpenFile(FileRecord<Strategy::NO_CACHE>& file,
              const FileRecord<Strategy::NO_CACHE>& dir, std::string_view name)
{
  const std::optional<IndexEntry> entry = dir.FindSubEntry(Widen(name));
  REQUIRE(entry.has_value());

  file.SetAttrMask(Mask::DATA);
  REQUIRE(file.ParseFileRecord(entry->GetFileReference()));
  REQUIRE(file.ParseAttrs());
}

std::vector<BYTE> ReadFile(const NtfsVolume<Strategy::NO_CACHE>& volume,
                           const FileRecord<Strategy::NO_CACHE>& dir,
                           std::string_view name)
{
  FileRecord<Strategy::NO_CACHE> file(volume);
  OpenFile(file, dir, name);

  const AttrBase<Strategy::NO_CACHE>* stream = file.FindStream({});
  REQUIRE(stream != nullptr);

  std::vector<BYTE> data(stream->GetDataSize());
  REQUIRE(stream->ReadData(0, data) == data.size());
  return data;
}

}  // namespace NtfsBrowserTests
