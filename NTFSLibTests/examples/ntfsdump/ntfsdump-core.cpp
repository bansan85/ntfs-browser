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

// Finds the next path separator at or after from. Returns -1 when none is
// left, as CString::Find does.
int FindSeparator(std::wstring_view path, std::size_t from) {
  const std::size_t pos = path.find(L'\\', from);
  return pos == std::wstring_view::npos ? -1 : static_cast<int>(pos);
}

}  // namespace

DumpResult DumpFile(NtfsVolume<Cache::Strategy::NoCache>& volume,
                    std::wstring_view path) {
  DumpResult result;

  if (!volume.IsVolumeOK()) {
    result.message = L"Not a valid NTFS volume or NTFS version < 3.0";
    return result;
  }

  // parse root directory

  Io::FileRecord<Cache::Strategy::NoCache> fr(volume);
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

  const std::wstring filename(path.substr(static_cast<std::size_t>(dirs + 1)));
  std::optional<IndexEntry> ie = fr.FindSubEntry(filename);
  if (!ie) {
    result.message = L"File not found\n";
    return result;
  }

  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    result.message = L"Cannot read file";
    return result;
  }

  // We only need DATA attribute and StdInfo
  fr.SetAttrMask(Attr::Mask::Data);
  if (!fr.ParseAttrs()) {
    result.message = L"Cannot parse file attributes";
    return result;
  }

  std::vector<BYTE> filebuf;
  filebuf.resize(kBufferSize);

  // only pick the unnamed stream (file data)
  const Attr::AttrBase<Cache::Strategy::NoCache>* data = fr.FindStream({});
  if (data != nullptr) {
    // show only the first 16K
    const ULONGLONG datalen = std::min(data->GetDataSize(), kBufferSize);

    std::optional<ULONGLONG> len =
        data->ReadData(0, {filebuf.data(), static_cast<std::size_t>(datalen)});
    if (!len || *len != datalen) {
      result.message = L"Read data error";
      return result;
    }

    ShowData(result.dump, filebuf.data(), static_cast<std::uint32_t>(datalen));
  }

  return result;
}

}  // namespace NtfsDump
