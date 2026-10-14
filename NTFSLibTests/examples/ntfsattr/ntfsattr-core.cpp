#include "ntfsattr-core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// Name the drive-only path resolves to. The root directory lists itself as
// "." in its index.
constexpr std::wstring_view kRootEntry = L".";

// Record handle the walk parses into.
using Record = Io::FileRecord<Cache::Strategy::FullCache>;

// Message of a failed step, or nullopt when the step went through.
using Failure = std::optional<std::wstring>;

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

// The heading of an attribute: its type name, then its stream name in
// parentheses when it has one. Each line ends in CRLF.
std::wstring
    AttrHeading(const Attr::AttrBase<Cache::Strategy::FullCache>& attr) {
  std::wstring heading = L"\r\n";
  heading += AttrNames[(static_cast<DWORD>(attr.GetAttrType()) >> 4U) - 1];

  const std::wstring_view attrname = attr.GetAttrName();
  if (!attrname.empty()) {
    heading += L'(';
    heading += attrname;
    heading += L')';
  }
  heading += L"\r\n";
  return heading;
}

// Traverse callback: appends one attribute's heading and raw header to the
// std::wstring that context points to.
void PrintAttr(const Attr::AttrBase<Cache::Strategy::FullCache>& attr,
               void* context, bool* /* stop*/) {
  auto* dump = static_cast<std::wstring*>(context);
  *dump += AttrHeading(attr);
  AppendData(*dump, reinterpret_cast<const BYTE*>(&attr.GetAttrHeader()),
             attr.GetAttrTotalSize());
}

// The directory names on path, in order. The drive and the last component are
// left out.
std::vector<std::wstring_view> DirectoryNames(std::wstring_view path) {
  std::vector<std::wstring_view> names;
  std::size_t dirs = path.find(L'\\');
  if (dirs == std::wstring_view::npos) {
    return names;
  }

  std::size_t dire = path.find(L'\\', dirs + 1);
  while (dire != std::wstring_view::npos) {
    names.push_back(path.substr(dirs + 1, dire - dirs - 1));
    dirs = dire;
    dire = path.find(L'\\', dirs + 1);
  }
  return names;
}

// The last component of path, which names the entry to dump. A drive-only
// path names the root directory.
std::wstring_view EntryName(std::wstring_view path) {
  const std::size_t last = path.rfind(L'\\');
  const std::wstring_view name =
      last == std::wstring_view::npos ? path : path.substr(last + 1);
  if (name.size() == 2 && name.find(L':') != std::wstring_view::npos) {
    return kRootEntry;
  }
  return name;
}

// Parses the root directory, with only the index attributes a walk needs.
Failure OpenRoot(Record& fr) {
  // we only need to parse INDEX_ROOT and INDEX_ALLOCATION
  // don't waste time and ram to parse unwanted attributes
  fr.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root))) {
    return L"Cannot read root directory of volume";
  }
  if (!fr.ParseAttrs()) {
    return L"Cannot parse attributes";
  }
  return std::nullopt;
}

// Steps from the directory fr holds into its child name, and parses the
// child's attributes. The root wording on a failed read is the one the sample
// has always shown here.
Failure EnterDirectory(Record& fr, std::wstring_view name) {
  const std::optional<IndexEntry> ie = fr.FindSubEntry(name);
  if (!ie) {
    return L"File not found\n";
  }
  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    return L"Cannot read root directory of volume";
  }
  if (!fr.ParseAttrs()) {
    return L"Cannot parse directory attributes";
  }
  return std::nullopt;
}

// Parses the entry name of the directory fr holds, with all its attributes.
Failure OpenEntry(Record& fr, std::wstring_view name) {
  const std::optional<IndexEntry> ie = fr.FindSubEntry(name);
  if (!ie) {
    return L"File not found\n";
  }
  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    return L"Cannot read file";
  }
  // parse all attributes
  fr.SetAttrMask(Attr::Mask::All);
  if (!fr.ParseAttrs()) {
    return L"Cannot parse file attributes";
  }
  return std::nullopt;
}

// A result that carries only the message to show.
DumpResult Failed(std::wstring_view message) {
  return DumpResult{.message = std::wstring(message)};
}

}  // namespace

void AppendData(std::wstring& lines, const BYTE* data, std::uint32_t datalen) {
  // Zero bytes: datalen - 1 would wrap and the row loop would read past data.
  if (datalen == 0) {
    return;
  }

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
  if (!volume.IsVolumeOK()) {
    return Failed(L"Not a valid NTFS volume or NTFS version < 3.0");
  }

  Record fr(volume);
  if (const Failure error = OpenRoot(fr)) {
    return Failed(*error);
  }

  for (const std::wstring_view name : DirectoryNames(path)) {
    if (const Failure error = EnterDirectory(fr, name)) {
      return Failed(*error);
    }
  }

  if (const Failure error = OpenEntry(fr, EntryName(path))) {
    return Failed(*error);
  }

  DumpResult result;
  fr.TraverseAttrs(PrintAttr, &result.dump);
  return result;
}

}  // namespace NtfsAttrDump
