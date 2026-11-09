#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <ntfs-browser/example/ntfsdir/arguments.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/tool/device-access/volume-target.h>
#include <ntfs-browser/types.h>

#include "utf16-convert.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir {

namespace {

// The most digits of a number that can still fit in 64 bits.
constexpr std::size_t max_number_digits = 20;

// The value of an option written as "--name=value", or nothing when arg is not
// that option.
std::optional<std::wstring_view> ValueOf(std::wstring_view arg,
                                         std::wstring_view name) {
  if (!arg.starts_with(name) || arg.size() == name.size() ||
      arg[name.size()] != L'=') {
    return std::nullopt;
  }
  return arg.substr(name.size() + 1);
}

// The engine that text names, or nothing for an unknown name.
std::optional<NTFS_BROWSER_NAMESPACE::EngineKind>
    EngineOf(std::wstring_view text) {
  if (text == L"auto") {
    return NTFS_BROWSER_NAMESPACE::EngineKind::Auto;
  }
  if (text == L"mftlist") {
    return NTFS_BROWSER_NAMESPACE::EngineKind::MftList;
  }
  if (text == L"nocache") {
    return NTFS_BROWSER_NAMESPACE::EngineKind::DirectoryNoCache;
  }
  if (text == L"fullcache") {
    return NTFS_BROWSER_NAMESPACE::EngineKind::DirectoryFullCache;
  }
  return std::nullopt;
}

// A number of decimal digits. Nothing for an empty text, any other character,
// or a value that does not fit.
std::optional<std::uint64_t> NumberOf(std::wstring_view text) {
  if (text.empty() || text.size() > max_number_digits) {
    return std::nullopt;
  }
  std::string digits;
  for (const wchar_t character : text) {
    if (character < L'0' || character > L'9') {
      return std::nullopt;
    }
    digits.push_back(static_cast<char>(character));
  }
  std::uint64_t value = 0;
  const char* const end = digits.data() + digits.size();
  const auto [stop, error] = std::from_chars(digits.data(), end, value);
  if (error != std::errc{} || stop != end) {
    return std::nullopt;
  }
  return value;
}

// What one option of the command line did.
enum class Handled : std::uint8_t {
  // arg is not an option of this program.
  NotMine,
  // arg was applied.
  Applied,
  // arg is an option of this program with a value that cannot be used. error
  // says why.
  Rejected
};

// Sets the pattern kind and text of --name or --regex. Rejects an empty text
// and the use of both options.
Handled SetPattern(NTFS_BROWSER_NAMESPACE::PatternKind kind,
                   std::wstring_view option, std::wstring_view text,
                   NTFS_BROWSER_NAMESPACE::Pattern& pattern,
                   std::wstring& error) {
  if (text.empty()) {
    error = std::wstring(option) + L" needs a pattern";
    return Handled::Rejected;
  }
  if (pattern.kind != NTFS_BROWSER_NAMESPACE::PatternKind::None &&
      pattern.kind != kind) {
    error = L"--name and --regex cannot be used together";
    return Handled::Rejected;
  }
  pattern.kind = kind;
  pattern.text = ToUtf16(text);
  return Handled::Applied;
}

// Reads the options that set a bound or a pattern: --name, --regex, --min-size
// and --max-size.
Handled ParseValueOption(std::wstring_view arg, Arguments& args,
                         std::wstring& error) {
  NTFS_BROWSER_NAMESPACE::Pattern& pattern = args.scan.pattern;
  if (const auto text = ValueOf(arg, L"--name")) {
    return SetPattern(NTFS_BROWSER_NAMESPACE::PatternKind::Wildcard, L"--name",
                      *text, pattern, error);
  }
  if (const auto text = ValueOf(arg, L"--regex")) {
    if (!NTFS_BROWSER_NAMESPACE::RegexAvailable()) {
      error =
          L"--regex is not available: this build has no regular expression "
          L"support";
      return Handled::Rejected;
    }
    return SetPattern(NTFS_BROWSER_NAMESPACE::PatternKind::Regex, L"--regex",
                      *text, pattern, error);
  }
  for (const std::wstring_view name : {L"--min-size", L"--max-size"}) {
    const auto text = ValueOf(arg, name);
    if (!text) {
      continue;
    }
    const std::optional<std::uint64_t> number = NumberOf(*text);
    if (!number) {
      error = std::wstring(name) + L" needs a number of bytes, not \"" +
              std::wstring(*text) + L"\"";
      return Handled::Rejected;
    }
    auto& bound = name == L"--min-size" ? args.scan.filter.size.min
                                        : args.scan.filter.size.max;
    bound = *number;
    return Handled::Applied;
  }
  return Handled::NotMine;
}

// Reads the options of this program. A value that cannot be used is
// Rejected, with the reason in error.
Handled ParseOwnOption(std::wstring_view arg, Arguments& args,
                       std::wstring& error) {
  NTFS_BROWSER_NAMESPACE::Options& scan = args.scan;
  if (arg == L"--recursive" || arg == L"-r") {
    scan.recursive = true;
  } else if (arg == L"--system") {
    scan.include_system_files = true;
  } else if (arg == L"--progress") {
    args.progress = true;
  } else if (arg == L"--match-path") {
    scan.pattern.target = NTFS_BROWSER_NAMESPACE::MatchTarget::FullPath;
  } else if (arg == L"--case") {
    scan.pattern.case_sensitive = true;
  } else if (arg == L"--dirs-only" || arg == L"--files-only") {
    const bool directories = arg == L"--dirs-only";
    if (scan.filter.is_directory && *scan.filter.is_directory != directories) {
      error = L"--dirs-only and --files-only cannot be used together";
      return Handled::Rejected;
    }
    scan.filter.is_directory = directories;
  } else if (const auto engine = ValueOf(arg, L"--engine")) {
    const std::optional<NTFS_BROWSER_NAMESPACE::EngineKind> kind =
        EngineOf(*engine);
    if (!kind) {
      error = L"Unknown engine \"" + std::wstring(*engine) +
              L"\": use auto, mftlist, nocache or fullcache";
      return Handled::Rejected;
    }
    scan.engine = *kind;
  } else {
    return ParseValueOption(arg, args, error);
  }
  return Handled::Applied;
}

}  // namespace

std::string UsageText() {
  std::string usage =
      "Usage: ntfsdir [options] \"target\"\n"
      "Lists the files of a folder, or of a whole volume, with Scan.\n"
      "\n"
      "Options:\n"
      "  --recursive, -r  also list every folder below the target folder\n"
      "  --engine=<auto|mftlist|nocache|fullcache>\n"
      "                   how the scan walks the volume. auto (the default)\n"
      "                   reads a folder through its indexes (fullcache),\n"
      "                   and a whole volume with -r in $MFT order\n"
      "                   (mftlist). nocache is fullcache with less memory.\n"
      "                   mftlist on a volume lists every file, with or\n"
      "                   without -r.\n"
      "  --name=PATTERN   only the names that match. The whole name must\n"
      "                   match. * stands for any run of characters and ?\n"
      "                   for one character. Case is ignored.\n";
  usage += NTFS_BROWSER_NAMESPACE::RegexAvailable()
               ? "  --regex=PATTERN  only the names in which a regular "
                 "expression (PCRE2)\n"
                 "                   matches. ^ and $ hold it to the start "
                 "and to the end.\n"
               : "  --regex=PATTERN  not available: this build has no regular "
                 "expressions\n";
  usage +=
      "  --match-path     test --name and --regex against the full path\n"
      "                   (\\Docs\\a.txt) instead of the name\n"
      "  --case           compare the pattern with case\n"
      "  --dirs-only      list folders only\n"
      "  --files-only     list files only\n"
      "  --min-size=N     list entries of at least N bytes\n"
      "  --max-size=N     list entries of at most N bytes\n"
      "  --system         also list the system files ($MFT, $Bitmap, ...)\n"
      "  --progress       write the percent of the scan to stderr (only\n"
      "                   mftlist knows it)\n";
  usage += Tool::DeviceAccess::CommonOptionsUsage();
  usage +=
      "\n"
      "Each line holds the write time, <DIR> or the size in bytes, the\n"
      "attributes (R read only, H hidden, S system) and the name. With\n"
      "--deleted a marker comes before the name: * for a deleted file, ? for\n"
      "a name whose file record cannot be read. The last line holds the\n"
      "totals.\n"
      "\n"
      "A recursive listing by folder (fullcache, nocache) comes under a\n"
      "\"Directory of\" header for each folder. The mftlist engine delivers\n"
      "the entries in $MFT order, so it writes the full path on each line\n"
      "instead of the name. Paths use '\\' as NTFS does.\n"
      "\n"
      "The exit code is 0 when the scan finishes and 1 otherwise.\n"
      "\n"
      "target is an image file or a device, followed by an optional folder\n"
      "inside the volume";
#ifdef _WIN32
  usage += ", or a drive path.\n";
  usage += "eg. ntfsdir c:\n";
  usage += "eg. ntfsdir -r c:\\windows --name=*.dll --min-size=1048576\n";
  usage += "eg. ntfsdir c:\\image.img\\Windows --deleted\n";
#else
  usage += ".\n";
  usage += "eg. ntfsdir /dev/sda1\n";
  usage += "eg. ntfsdir -r /dev/sda1/Windows --name=*.dll --min-size=1048576\n";
  usage += "eg. ntfsdir ./disk.img/Windows --deleted\n";
#endif
  return usage;
}

std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[],
                                        std::wstring& error) {
  Arguments args;
  bool has_target = false;
  for (int i = 1; i < argc; i++) {
    const std::wstring_view arg = argv[i];
    const Tool::DeviceAccess::OptionResult common =
        Tool::DeviceAccess::ParseCommonOption(arg, args.common);
    if (common == Tool::DeviceAccess::OptionResult::Malformed) {
      error = L"Malformed option " + std::wstring(arg);
      return std::nullopt;
    }
    if (common == Tool::DeviceAccess::OptionResult::Accepted) {
      continue;
    }

    const Handled own = ParseOwnOption(arg, args, error);
    if (own == Handled::Rejected) {
      return std::nullopt;
    }
    if (own == Handled::Applied) {
      continue;
    }
    if (arg.starts_with(L"-") && arg.size() > 1) {
      error = L"Unknown option " + std::wstring(arg);
      return std::nullopt;
    }
    if (has_target) {
      error = L"Only one target is allowed";
      return std::nullopt;
    }
    args.scan.target = ToUtf16(arg);
    has_target = true;
  }

  if (args.common.help) {
    return args;
  }
  if (!has_target) {
    error = L"A target is needed";
    return std::nullopt;
  }
  if (args.scan.filter.size.IsEmpty()) {
    error = L"--min-size is above --max-size";
    return std::nullopt;
  }
  args.scan.include_deleted = args.common.volume_options.include_deleted;
  args.scan.recover_errors = args.common.volume_options.recover_errors;
  return args;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir
