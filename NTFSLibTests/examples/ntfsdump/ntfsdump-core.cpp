#include "ntfsdump-core.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>

using namespace NtfsBrowser;

namespace NtfsDump {

namespace {

// Bytes one dump shows. Longer files are cut here.
constexpr std::uint64_t kBufferSize = 16U * 1024U;

// Record handle the walk parses into.
using Record = Io::FileRecord<Cache::Strategy::NoCache>;

// Formats one 16-byte row: address, hex, then the text column. Bytes under
// 0x20 show as '.'.
std::wstring FormatRow(std::uint32_t address, const std::array<BYTE, 16>& row) {
  std::wstring line = std::format(L"{:04X}    ", address);
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

// Appends the dump of datalen bytes at data to dump. Every row but the last
// ends in CRLF.
void ShowData(std::wstring& dump, const BYTE* data, std::uint32_t datalen) {
  if (datalen == 0) {
    return;
  }

  std::uint32_t i = 0;
  for (i = 0; i < ((datalen - 1) >> 4U); i++) {
    std::array<BYTE, 16> row{};
    std::memcpy(row.data(), data + static_cast<std::size_t>(i) * 16, 16);
    dump += FormatRow(i * 16, row) + L"\r\n";
  }

  // The last row may be short. The missing bytes print as 0xFF.
  std::array<BYTE, 16> row{};
  row.fill(0xFF);
  std::memcpy(row.data(), data + static_cast<std::size_t>(i) * 16,
              datalen - i * 16);
  dump += FormatRow(i * 16, row);
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

// The last component of path, which names the file to dump.
std::wstring_view LastComponent(std::wstring_view path) {
  const std::size_t last = path.rfind(L'\\');
  return last == std::wstring_view::npos ? path : path.substr(last + 1);
}

// Message of a failed step, or nullopt when the step went through.
using Failure = std::optional<std::wstring>;

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

// Parses the file name of the directory fr holds, with only its unnamed DATA
// attribute.
Failure OpenFile(Record& fr, std::wstring_view name) {
  const std::optional<IndexEntry> ie = fr.FindSubEntry(name);
  if (!ie) {
    return L"File not found\n";
  }
  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    return L"Cannot read file";
  }
  fr.SetAttrMask(Attr::Mask::Data);
  if (!fr.ParseAttrs()) {
    return L"Cannot parse file attributes";
  }
  return std::nullopt;
}

// Appends the first kBufferSize bytes of the unnamed stream of the parsed file
// to dump. A file with no unnamed stream adds nothing.
Failure DumpStart(const Record& fr, std::wstring& dump) {
  // only pick the unnamed stream (file data)
  const Attr::AttrBase<Cache::Strategy::NoCache>* data = fr.FindStream({});
  if (data == nullptr) {
    return std::nullopt;
  }

  std::vector<BYTE> filebuf(kBufferSize);
  // show only the first 16K
  const ULONGLONG datalen = std::min(data->GetDataSize(), kBufferSize);
  const std::optional<ULONGLONG> len =
      data->ReadData(0, {filebuf.data(), static_cast<std::size_t>(datalen)});
  if (!len || *len != datalen) {
    return L"Read data error";
  }

  ShowData(dump, filebuf.data(), static_cast<std::uint32_t>(datalen));
  return std::nullopt;
}

// A result that carries only the message to show.
DumpResult Failed(std::wstring_view message) {
  return DumpResult{.message = std::wstring(message)};
}

}  // namespace

DumpResult DumpFile(NtfsVolume<Cache::Strategy::NoCache>& volume,
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

  if (const Failure error = OpenFile(fr, LastComponent(path))) {
    return Failed(*error);
  }

  DumpResult result;
  if (const Failure error = DumpStart(fr, result.dump)) {
    return Failed(*error);
  }
  return result;
}

}  // namespace NtfsDump
