#include "ntfsattr-core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>

using namespace NtfsBrowser;

namespace NtfsAttrDump {

namespace {

// One name per attribute type: 0x10 (STANDARD_INFORMATION) to 0x100
// (LOGGED_UTILITY_STREAM), in steps of 0x10.
constexpr std::size_t kAttrNameCount = 16;

const std::array<std::wstring_view, kAttrNameCount> AttrNames = {
    L"STANDARD_INFORMATION",
    L"ATTRIBUTE_LIST",
    L"FILE_NAME",
    L"OBJECT_ID",
    L"SECURITY_DESCRIPTOR",
    L"VOLUME_NAME",
    L"VOLUME_INFORMATION",
    L"DATA",
    L"INDEX_ROOT",
    L"INDEX_ALLOCATION",
    L"BITMAP",
    L"REPARSE_POINT",
    L"EA_INFORMATION",
    L"EA",
    L"NULL",
    L"LOGGED_UTILITY_STREAM"};

// Formats one 16-byte row: hex, then the text column. Bytes under 0x20 show as
// '.'.
std::wstring FormatRow(const std::array<BYTE, 16>& row) {
  std::wstring line;
  for (std::size_t j = 0; j < row.size(); ++j) {
    line += std::format(L"{:02X}", row[j]);
    if (j == 7) {
      line += L" - ";
    } else if (j == 15) {
      line += L"   ";
    } else {
      line += L' ';
    }
  }
  for (const BYTE byte : row) {
    line += static_cast<wchar_t>(byte < 0x20 ? '.' : byte);
  }
  return line;
}

// Appends the dump of datalen bytes at data to lines. Every row ends in CRLF.
void AppendData(std::wstring& lines, const BYTE* data, std::uint32_t datalen) {
  std::uint32_t i = 0;
  for (i = 0; i < ((datalen - 1U) >> 4); i++) {
    std::array<BYTE, 16> row{};
    std::memcpy(row.data(), data + static_cast<std::size_t>(i) * 16, 16);
    lines += FormatRow(row) + L"\r\n";
  }

  // The last row may be short. The missing bytes print as 0xFF.
  std::array<BYTE, 16> row{};
  row.fill(0xFF);
  std::memcpy(row.data(), data + static_cast<std::size_t>(i) * 16,
              datalen - i * 16);
  lines += FormatRow(row) + L"\r\n";
}

// Traverse callback: appends one attribute's name and raw header to the
// std::wstring that context points to.
void PrintAttr(const Attr::AttrBase<Cache::Strategy::FullCache>& attr,
               void* context, bool* /* stop*/) {
  auto* dump = static_cast<std::wstring*>(context);

  std::wstring line = L"\r\n";
  line += AttrNames[(static_cast<DWORD>(attr.GetAttrType()) >> 4U) - 1];

  std::wstring_view attrname = attr.GetAttrName();
  if (!attrname.empty()) {
    line += L'(';
    line += attrname;
    line += L')';
  }
  line += L"\r\n";

  AppendData(line, reinterpret_cast<const BYTE*>(&attr.GetAttrHeader()),
             attr.GetAttrTotalSize());

  *dump += line;
}

// Finds the next path separator at or after from. Returns -1 when none is
// left, as CString::Find does.
int FindSeparator(std::wstring_view path, std::size_t from) {
  const std::size_t pos = path.find(L'\\', from);
  return pos == std::wstring_view::npos ? -1 : static_cast<int>(pos);
}

}  // namespace

std::wstring TargetPath(std::wstring_view picked, bool directory) {
  if (!directory) {
    return std::wstring(picked);
  }
  const std::size_t slash = picked.rfind(L'\\');
  if (slash == std::wstring_view::npos) {
    return {};
  }
  return std::wstring(picked.substr(0, slash));
}

DumpResult DumpAttributes(NtfsVolume<Cache::Strategy::FullCache>& volume,
                          std::wstring_view path) {
  DumpResult result;

  if (!volume.IsVolumeOK()) {
    result.message = L"Not a valid NTFS volume or NTFS version < 3.0";
    return result;
  }

  // parse root directory

  Io::FileRecord<Cache::Strategy::FullCache> fr(volume);
  // we only need to parse INDEX_ROOT and INDEX_ALLOCATION
  // don't waste time and ram to parse unwanted attributes
  fr.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root))) {
    result.message = L"Cannot read root directory of volume";
    return result;
  }

  if (!fr.ParseAttrs()) {
    result.message = L"Cannot parse attributes";
    return result;
  }

  // find subdirectory

  int dirs = FindSeparator(path, 0);
  int dire = FindSeparator(path, static_cast<std::size_t>(dirs + 1));
  while (dire != -1) {
    const std::wstring pathname(
        path.substr(static_cast<std::size_t>(dirs + 1),
                    static_cast<std::size_t>(dire - dirs - 1)));

    std::optional<IndexEntry> ie = fr.FindSubEntry(pathname);
    if (!ie) {
      result.message = L"File not found\n";
      return result;
    }

    if (!fr.ParseFileRecord(ie->GetFileReference())) {
      result.message = L"Cannot read root directory of volume";
      return result;
    }

    if (!fr.ParseAttrs()) {
      result.message = L"Cannot parse directory attributes";
      return result;
    }

    dirs = dire;
    dire = FindSeparator(path, static_cast<std::size_t>(dirs + 1));
  }

  // dump it !

  std::wstring filename(path.substr(static_cast<std::size_t>(dirs + 1)));

  // root directory
  if (filename.size() == 2 && filename.find(L':') != std::wstring::npos) {
    filename = L".";
  }

  std::optional<IndexEntry> ie = fr.FindSubEntry(filename);
  if (!ie) {
    result.message = L"File not found\n";
    return result;
  }

  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    result.message = L"Cannot read file";
    return result;
  }

  // parse all attributes
  fr.SetAttrMask(Attr::Mask::All);
  if (!fr.ParseAttrs()) {
    result.message = L"Cannot parse file attributes";
    return result;
  }

  fr.TraverseAttrs(PrintAttr, &result.dump);
  return result;
}

}  // namespace NtfsAttrDump
