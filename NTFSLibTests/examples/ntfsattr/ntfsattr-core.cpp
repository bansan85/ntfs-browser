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

#include <ntfs-browser/example/ntfsattr/ntfsattr-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/tool-support.h>
#include <ntfs-browser/tool/device-access/volume-target.h>

#include "attr/base.h"
#include "index-entry.h"
#include "io/file-record.h"
#include "log/log.h"
#include "mft/idx.h"

using namespace NTFS_BROWSER_NAMESPACE;

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsAttr {

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

// Name the empty path resolves to. The root directory lists itself as "." in
// its index.
constexpr std::wstring_view kRootEntry = L".";

// Bytes in one row of the dump.
constexpr std::size_t kRowBytes = 16;

// Record handle the walk parses into.
using Record = Io::FileRecord<Cache::Strategy::FullCache>;

// Message of a failed step, or nullopt when the step went through.
using Failure = std::optional<std::wstring>;

// Formats one row of count bytes: hex, then the text column. A byte outside
// the printable ASCII range shows as '.'. The hex of missing bytes is blank.
std::wstring FormatRow(const std::array<BYTE, kRowBytes>& row,
                       std::size_t count) {
  std::wstring line;
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

// The heading of an attribute: its type name, then its stream name in
// parentheses when it has one. The line ends in LF.
std::wstring
    AttrHeading(const Attr::AttrBase<Cache::Strategy::FullCache>& attr) {
  std::wstring heading = AttrTypeName(attr.GetAttrType());

  const std::wstring_view attrname = attr.GetAttrName();
  if (!attrname.empty()) {
    heading += L'(';
    heading += attrname;
    heading += L')';
  }
  heading += L"\n";
  return heading;
}

// Traverse callback: appends one attribute's heading and raw header to the
// std::wstring that context points to. A blank line sets each attribute apart
// from the one before.
void PrintAttr(const Attr::AttrBase<Cache::Strategy::FullCache>& attr,
               void* context, bool* /* stop*/) {
  auto* dump = static_cast<std::wstring*>(context);
  if (!dump->empty()) {
    *dump += L"\n";
  }
  *dump += AttrHeading(attr);
  AppendData(*dump, reinterpret_cast<const BYTE*>(&attr.GetAttrHeader()),
             attr.GetAttrTotalSize());
}

// The name of the entry to dump: the last part of path. The root directory
// lists itself as kRootEntry.
std::wstring_view EntryName(const std::vector<std::wstring_view>& parts) {
  return parts.empty() ? kRootEntry : parts.back();
}

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

// Parses the entry name of the directory fr holds, with all its attributes.
Failure OpenEntry(Record& fr, std::wstring_view name) {
  const std::optional<IndexEntry> ie = fr.FindSubEntry(name);
  if (!ie) {
    return L"File not found";
  }
  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    return L"Cannot read file";
  }
  // parse all attributes
  fr.SetParseOptions({.attr_mask = Attr::Mask::All});
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
  for (std::size_t offset = 0; offset < datalen; offset += kRowBytes) {
    const std::size_t count =
        std::min<std::size_t>(kRowBytes, datalen - offset);
    std::array<BYTE, kRowBytes> row{};
    std::copy_n(data + offset, count, row.begin());
    lines += FormatRow(row, count) + L"\n";
  }
}

std::wstring AttrTypeName(NTFS_BROWSER_NAMESPACE::Attr::Type type) {
  const auto raw = static_cast<DWORD>(type);
  // Only the multiples of 0x10 in the named range index AttrNames. Any other
  // value, 0 included, is shown in hex.
  if (raw < static_cast<DWORD>(Attr::Type::StandardInformation) ||
      raw > static_cast<DWORD>(Attr::Type::LoggedUtilityStream) ||
      (raw & 0xFU) != 0) {
    return std::format(L"0x{:04X}", raw);
  }
  return std::wstring(AttrNames[(raw >> 4U) - 1]);
}

std::wstring TargetPath(std::wstring_view picked, bool directory) {
  if (!directory) {
    return std::wstring(picked);
  }
  const std::vector<std::wstring_view> parts =
      Tool::DeviceAccess::SplitInsidePath(picked);
  std::wstring parent;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    if (i != 0) {
      parent += L'\\';
    }
    parent += parts[i];
  }
  return parent;
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

  const std::vector<std::wstring_view> parts =
      Tool::DeviceAccess::SplitInsidePath(path);
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    if (const Failure error = EnterDirectory(fr, parts[i])) {
      return Failed(*error);
    }
  }

  if (const Failure error = OpenEntry(fr, EntryName(parts))) {
    return Failed(*error);
  }

  DumpResult result;
  fr.TraverseAttrs(PrintAttr, &result.dump);
  return result;
}

std::string UsageText() {
  std::string usage =
      "Usage: ntfsattr [--directory] [--deleted] [--recover] [--log=...] "
      "\"target\"\n"
      "Dumps the raw header of every attribute of a file or directory.\n"
      "  --directory, -d  dump the directory that holds target instead\n";
  usage += Tool::DeviceAccess::CommonOptionsUsage();
  usage +=
      "target is an image file or a device, followed by the path inside the "
      "volume";
#ifdef _WIN32
  usage += ", or a drive path.\n";
  usage += "eg. ntfsattr c:\\windows\\notepad.exe\n";
  usage += "eg. ntfsattr --directory c:\\windows\\notepad.exe\n";
  usage += "eg. ntfsattr c:\\image.img\\Windows\n";
#else
  usage += ".\n";
  usage += "eg. ntfsattr /dev/sda1/Windows/notepad.exe\n";
  usage += "eg. ntfsattr --directory /dev/sda1/Windows/notepad.exe\n";
  usage += "eg. ntfsattr ./disk.img/Windows\n";
#endif
  return usage;
}

std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[]) {
  Arguments args;
  bool has_target = false;
  for (int i = 1; i < argc; i++) {
    const std::wstring_view arg = argv[i];
    if (arg == L"--directory" || arg == L"-d") {
      args.directory = true;
      continue;
    }
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
      Tool::DeviceAccess::OpenVolumeFromTarget<Cache::Strategy::FullCache>(
          args.target, args.common.volume_options);
  if (!opened.volume) {
    Tool::DeviceAccess::WriteText(err, opened.error + L"\n");
    return failure_exit_code;
  }

  const DumpResult result =
      DumpAttributes(*opened.volume, TargetPath(opened.inside, args.directory));
  if (!result.message.empty()) {
    Tool::DeviceAccess::WriteText(err, result.message + L"\n");
    return failure_exit_code;
  }
  Tool::DeviceAccess::WriteText(out, result.dump);
  return 0;
}

int RunNtfsAttr(int argc, wchar_t* argv[]) {
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

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsAttr
