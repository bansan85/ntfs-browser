#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/example/ntfsdump/ntfsdump-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/tool-support.h>
#include <ntfs-browser/tool/device-access/volume-target.h>

#include "attr/base.h"
#include "index-entry.h"
#include "io/file-record.h"
#include "log/log.h"
#include "mft/idx.h"

using namespace NTFS_BROWSER_NAMESPACE;

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDump {

namespace {

// Bytes one dump shows. Longer files are cut here.
constexpr std::uint64_t kBufferSize = 16U * 1024U;

// Bytes in one row of the dump.
constexpr std::size_t kRowBytes = 16;

// Name the empty path resolves to. The root directory lists itself as "." in
// its index.
constexpr std::wstring_view kRootEntry = L".";

// Record handle the walk parses into.
using Record = Io::FileRecord<Cache::Strategy::NoCache>;

// Formats one row of count bytes: address, hex, then the text column. A byte
// outside the printable ASCII range shows as '.'. The hex of missing bytes is
// blank.
std::wstring FormatRow(std::uint32_t address,
                       const std::array<BYTE, kRowBytes>& row,
                       std::size_t count) {
  std::wstring line = std::format(L"{:04X}    ", address);
  for (std::size_t j = 0; j < row.size(); ++j) {
    line += j < count ? std::format(L"{:02X}", row[j]) : std::wstring(2, L' ');
    if (j == 7) {
      line += L" - ";
    } else if (j == 15) {
      line += L"   ";
    } else {
      line += L' ';
    }
  }
  for (std::size_t j = 0; j < count; ++j) {
    line +=
        static_cast<wchar_t>(row[j] >= 0x20 && row[j] < 0x7F ? row[j] : '.');
  }
  return line;
}

// Appends the dump of datalen bytes at data to dump. Every row ends in LF.
void ShowData(std::wstring& dump, const BYTE* data, std::uint32_t datalen) {
  for (std::size_t offset = 0; offset < datalen; offset += kRowBytes) {
    const std::size_t count =
        std::min<std::size_t>(kRowBytes, datalen - offset);
    std::array<BYTE, kRowBytes> row{};
    std::copy_n(data + offset, count, row.begin());
    dump += FormatRow(static_cast<std::uint32_t>(offset), row, count) + L"\n";
  }
}

// The name of the file to dump: the last part of path. The root directory
// lists itself as kRootEntry.
std::wstring_view EntryName(const std::vector<std::wstring_view>& parts) {
  return parts.empty() ? kRootEntry : parts.back();
}

// Message of a failed step, or nullopt when the step went through.
using Failure = std::optional<std::wstring>;

// Parses the root directory, with only the index attributes a walk needs.
Failure OpenRoot(Record& fr) {
  // we only need to parse INDEX_ROOT and INDEX_ALLOCATION
  // don't waste time and ram to parse unwanted attributes
  fr.SetParseOptions(
      {.attr_mask = Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation});
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
    return L"File not found";
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
    return L"File not found";
  }
  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    return L"Cannot read file";
  }
  fr.SetParseOptions({.attr_mask = Attr::Mask::Data});
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

  const std::vector<std::wstring_view> parts =
      Tool::DeviceAccess::SplitInsidePath(path);
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    if (const Failure error = EnterDirectory(fr, parts[i])) {
      return Failed(*error);
    }
  }

  if (const Failure error = OpenFile(fr, EntryName(parts))) {
    return Failed(*error);
  }

  DumpResult result;
  if (const Failure error = DumpStart(fr, result.dump)) {
    return Failed(*error);
  }
  return result;
}

std::string UsageText() {
  std::string usage =
      "Usage: ntfsdump [--deleted] [--recover] [--log=...] \"target\"\n"
      "Dumps the first 16 KiB of the unnamed data stream of a file.\n";
  usage += Tool::DeviceAccess::CommonOptionsUsage();
  usage +=
      "target is an image file or a device, followed by the path inside the "
      "volume";
#ifdef _WIN32
  usage += ", or a drive path.\n";
  usage += "eg. ntfsdump c:\\windows\\notepad.exe\n";
  usage += "eg. ntfsdump c:\\image.img\\Windows\\notepad.exe\n";
#else
  usage += ".\n";
  usage += "eg. ntfsdump /dev/sda1/Windows/notepad.exe\n";
  usage += "eg. ntfsdump ./disk.img/Windows/notepad.exe\n";
#endif
  return usage;
}

std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[]) {
  Arguments args;
  bool has_target = false;
  for (int i = 1; i < argc; i++) {
    const std::wstring_view arg = argv[i];
    const Tool::DeviceAccess::OptionResult common =
        Tool::DeviceAccess::ParseCommonOption(arg, args.common);
    if (common == Tool::DeviceAccess::OptionResult::Malformed) {
      return std::nullopt;
    }
    if (common == Tool::DeviceAccess::OptionResult::Accepted) {
      continue;
    }
    if (has_target) {
      return std::nullopt;
    }
    args.target = arg;
    has_target = true;
  }

  if (!has_target && !args.common.help) {
    return std::nullopt;
  }
  return args;
}

int DumpTarget(const Arguments& args, std::FILE* out, std::FILE* err) {
  const auto opened =
      Tool::DeviceAccess::OpenVolumeFromTarget<Cache::Strategy::NoCache>(
          args.target, args.common.volume_options);
  if (!opened.volume) {
    Tool::DeviceAccess::WriteText(err, opened.error + L"\n");
    return failure_exit_code;
  }

  const DumpResult result = DumpFile(*opened.volume, opened.inside);
  if (!result.message.empty()) {
    Tool::DeviceAccess::WriteText(err, result.message + L"\n");
    return failure_exit_code;
  }
  Tool::DeviceAccess::WriteText(out, result.dump);
  return 0;
}

int RunNtfsDump(int argc, wchar_t* argv[]) {
  const std::optional<Arguments> args = ParseArguments(argc, argv);
  if (!args) {
    (void)std::fputs(UsageText().c_str(), stderr);
    return failure_exit_code;
  }
  if (args->common.help) {
    (void)std::fputs(UsageText().c_str(), stdout);
    return 0;
  }

  // Names print as UTF-8: a console shows them right with its code page set.
  const Tool::DeviceAccess::Utf8ConsoleOutput console;
  if (!Log::Configure(args->common.log_config)) {
    Tool::DeviceAccess::ReportLogFileError(args->common.log_config);
  }
  return DumpTarget(*args, stdout, stderr);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDump
