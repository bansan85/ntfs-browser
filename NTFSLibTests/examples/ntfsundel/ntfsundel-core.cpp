#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/example/ntfsundel/ntfsundel-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/tool/device-access/device-path.h>
#include <ntfs-browser/tool/device-access/local-time.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/types.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <cerrno>

  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace Lib = NTFS_BROWSER_NAMESPACE;

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel {

namespace {

namespace Device = Tool::DeviceAccess;

// Time between two polls of the scan, which are also two looks at the stop
// request. Short, so that a small volume does not wait: Poll() never blocks.
constexpr std::chrono::milliseconds kPollInterval(5);

// What the user is told when the data of a freed file probably belongs to
// another file now.
constexpr std::wstring_view kReallocatedMessage =
    L"The clusters of this file were given to another file after it was "
    L"deleted. The recovered data probably belongs to that other file.";

// What the user is told when clusters of a freed file are marked bad.
constexpr std::wstring_view kBadClustersMessage =
    L"Some clusters of this file are marked bad. The recovered data may be "
    L"incomplete, or the read may fail.";

// What the user is told when the allocation of a freed file could not be read.
// It is a warning only.
constexpr std::wstring_view kUncheckedMessage =
    L"The allocation of the clusters of this file could not be checked. The "
    L"recovered data may be wrong.";

// Largest time that a Windows FILETIME takes: the signed 64-bit range. Past it
// the date prints as zeros.
constexpr std::uint64_t kMaxSystemTimeTicks = 0x7FFFFFFFFFFFFFFFULL;

// The units of the high surrogates of UTF-16, and of the low ones. A code point
// above U+FFFF is written with a pair.
constexpr char32_t high_surrogate_base = 0xD800;
constexpr char32_t low_surrogate_base = 0xDC00;

// The first code point that UTF-16 writes as a pair.
constexpr char32_t first_supplementary = 0x10000;

// Mask of the ten bits that each half of a surrogate pair carries.
constexpr char32_t surrogate_bits = 0x3FF;

// Bits that a half of a surrogate pair holds.
constexpr unsigned surrogate_shift = 10;

// The options of a scan of the deleted files of target.
Lib::Options DeletedScanOptions(std::wstring_view target, bool recover_errors) {
  Lib::Options options;
  options.target = ToUtf16(target);
  options.engine = Lib::EngineKind::MftList;
  options.include_deleted = true;
  options.recover_errors = recover_errors;
  // A folder in the target lists everything below it.
  options.recursive = true;
  options.full_path = true;
  options.deleted_data_state = true;
  options.filter.deleted = Lib::DeletedFilter::OnlyDeleted;
  return options;
}

// The text of UTF-16 units as a wide string. A surrogate pair becomes one
// wchar_t where wchar_t holds a code point.
std::wstring Utf16ToWide(std::u16string_view units) {
  std::wstring text;
  text.reserve(units.size());
  for (std::size_t i = 0; i < units.size(); ++i) {
    const char32_t unit = units[i];
    const bool pair = unit >= high_surrogate_base &&
                      unit < low_surrogate_base && i + 1 < units.size() &&
                      units[i + 1] >= low_surrogate_base &&
                      units[i + 1] <= low_surrogate_base + surrogate_bits;
    if (sizeof(wchar_t) > sizeof(char16_t) && pair) {
      const char32_t low = units[++i];
      text.push_back(static_cast<wchar_t>(
          first_supplementary +
          (((unit - high_surrogate_base) & surrogate_bits) << surrogate_shift) +
          (low - low_surrogate_base)));
    } else {
      text.push_back(static_cast<wchar_t>(unit));
    }
  }
  return text;
}

// The row of the listing that entry makes.
DeletedFile MakeRow(const Lib::Entry& entry) {
  return {.handle = entry.GetHandle(),
          .reference = entry.record,
          .name = Utf16ToWide(entry.name),
          .full_path = Utf16ToWide(entry.path),
          .write_time = entry.modified.ticks};
}

}  // namespace

std::u16string ToUtf16(std::wstring_view text) {
  std::u16string units;
  units.reserve(text.size());
  for (const wchar_t character : text) {
    const auto code = static_cast<char32_t>(character);
    if (code < first_supplementary) {
      units.push_back(static_cast<char16_t>(code));
      continue;
    }
    const char32_t offset = code - first_supplementary;
    units.push_back(static_cast<char16_t>(
        high_surrogate_base + ((offset >> surrogate_shift) & surrogate_bits)));
    units.push_back(
        static_cast<char16_t>(low_surrogate_base + (offset & surrogate_bits)));
  }
  return units;
}

std::optional<SearchResult>
    ListDeletedFiles(std::wstring_view target, bool recover_errors,
                     const SearchControl& control,
                     const std::optional<Lib::Pattern>& pattern) {
  Lib::Options options = DeletedScanOptions(target, recover_errors);
  if (pattern) {
    options.pattern = *pattern;
  }

  SearchResult result;
  std::set<std::uint64_t> listed;
  Lib::Scan scan(options);
  scan.OnBatch([&](const Lib::Batch& batch) {
    for (const Lib::Entry& entry : batch.entries) {
      if (result.truncated) {
        return;
      }
      // A record with several names is one file: its first name stands.
      if (!listed.insert(entry.record).second) {
        continue;
      }
      result.files.push_back(MakeRow(entry));
      if (result.files.size() >= control.max_listed) {
        result.truncated = true;
        scan.Cancel();
      }
    }
  });
  scan.Start();
  while (!scan.Finished()) {
    if (control.stop_requested && control.stop_requested()) {
      scan.Cancel();
    }
    (void)scan.Poll();
    if (!scan.Finished()) {
      std::this_thread::sleep_for(kPollInterval);
    }
  }
  if (scan.GetState() == Lib::State::Failed) {
    return std::nullopt;
  }
  return result;
}

Lib::Pattern MakeNamePattern(std::wstring_view wildcard) {
  std::wstring text(wildcard);
  Lib::Pattern pattern;
  pattern.kind = Lib::PatternKind::Wildcard;
  if (text.find_first_of(L"\\/") == std::wstring::npos) {
    pattern.target = Lib::MatchTarget::Name;
  } else {
    pattern.target = Lib::MatchTarget::FullPath;
    std::ranges::replace(text, L'/', L'\\');
    if (!text.starts_with(L'\\')) {
      text.insert(0, L"*\\");
    }
  }
  pattern.text = ToUtf16(text);
  return pattern;
}

std::optional<std::set<std::uint64_t>>
    FindNamedFiles(std::wstring_view target, bool recover_errors,
                   std::wstring_view wildcard) {
  SearchControl control;
  control.max_listed = std::numeric_limits<std::size_t>::max();
  const std::optional<SearchResult> found = ListDeletedFiles(
      target, recover_errors, control, MakeNamePattern(wildcard));
  if (!found) {
    return std::nullopt;
  }
  std::set<std::uint64_t> records;
  for (const DeletedFile& file : found->files) {
    records.insert(file.reference);
  }
  return records;
}

std::wstring TooManyFilesMessage() {
  return std::format(L"Too many files found, only the first {} will be shown",
                     kMaxListedFiles);
}

std::wstring FormatLocalTime(std::uint64_t utc_ticks) {
  const std::optional<Device::LocalTime> time =
      utc_ticks > kMaxSystemTimeTicks ? std::nullopt
                                      : Device::UtcTicksToLocalTime(utc_ticks);
  if (!time) {
    return L"0000-00-00  00:00";
  }
  return std::format(L"{:04}-{:02}-{:02}  {:02}:{:02}", time->year, time->month,
                     time->day, time->hour, time->minute);
}

CopyResult CopyUnnamedData(
    Lib::Reader& reader, const Lib::Handle& handle,
    const std::function<bool(std::uint64_t size)>& confirm_large,
    const std::function<bool(std::span<const std::uint8_t> chunk)>& write) {
  const auto streams = reader.GetStreams(handle);
  if (!streams) {
    return {.error = RecoverError::Unreadable};
  }
  const auto unnamed =
      std::ranges::find_if(streams.value, [](const Lib::StreamInfo& stream) {
        return stream.name.empty();
      });
  if (unnamed == streams.value.end()) {
    return {.error = RecoverError::NoDataStream};
  }

  // Check files with huge size (maybe something is error)
  if (unnamed->size > kConfirmFileSize && !confirm_large(unnamed->size)) {
    return {.error = RecoverError::Declined};
  }

  bool write_failed = false;
  const auto copied =
      reader.CopyTo(handle, u"", [&](std::span<const std::uint8_t> chunk) {
        write_failed = !write(chunk);
        return !write_failed;
      });
  if (!copied) {
    return {.error = write_failed ? RecoverError::Write : RecoverError::Read};
  }
  return {.error = RecoverError::None, .bytes = copied.value};
}

RecoveryCheck AssessRecovery(Lib::DataState state) {
  switch (state) {
    case Lib::DataState::NotChecked:
    case Lib::DataState::Intact:
      return {};
    case Lib::DataState::Reallocated:
      return {.advice = RecoveryAdvice::Confirm,
              .cause = RecoveryCause::Reallocated};
    case Lib::DataState::Bad:
      return {.advice = RecoveryAdvice::Confirm,
              .cause = RecoveryCause::BadClusters};
    case Lib::DataState::Unknown:
      break;
  }
  return {.advice = RecoveryAdvice::Warn, .cause = RecoveryCause::Unchecked};
}

RecoveryCheck AssessRecovery(Lib::Reader& reader, const Lib::Handle& handle) {
  const auto state = reader.GetDataState(handle);
  return state ? AssessRecovery(state.value) : RecoveryCheck{};
}

std::wstring RecoveryMessage(const RecoveryCheck& check) {
  switch (check.cause) {
    case RecoveryCause::Reallocated:
      return std::wstring(kReallocatedMessage);
    case RecoveryCause::BadClusters:
      return std::wstring(kBadClustersMessage);
    case RecoveryCause::Unchecked:
      return std::wstring(kUncheckedMessage);
    case RecoveryCause::None:
      break;
  }
  return {};
}

bool ShouldRecover(const RecoveryCheck& check,
                   const std::function<bool(const RecoveryCheck&)>& confirm) {
  return check.advice == RecoveryAdvice::Proceed || confirm(check);
}

namespace {

// Characters Win32 refuses in a file name, besides those below U+0020.
constexpr std::wstring_view forbidden_name_characters = L"<>:\"/\\|?*";

// First character that is not a control code: the ones below it are refused.
constexpr wchar_t first_printable = 0x20;

// What replaces a character a name cannot hold, and the name of a recovery
// whose name is empty once cleaned.
constexpr wchar_t replacement_character = L'_';

// Device names Win32 reads as the device, with any extension.
constexpr std::array<std::wstring_view, 4> reserved_device_names{
    L"CON", L"PRN", L"AUX", L"NUL"};

// Device names that take a digit from 1 to 9: COM1, LPT9.
constexpr std::array<std::wstring_view, 2> numbered_device_names{L"COM",
                                                                 L"LPT"};

// The superscript digits (U+00B9, U+00B2, U+00B3) that Win32 also reads as a
// device number after COM or LPT.
constexpr std::array<wchar_t, 3> superscript_digits{0x00B9, 0x00B2, 0x00B3};

// The ranges of the UTF-16 high and low surrogates, which make a pair.
constexpr wchar_t high_surrogate_first = 0xD800;
constexpr wchar_t high_surrogate_last = 0xDBFF;
constexpr wchar_t low_surrogate_first = 0xDC00;
constexpr wchar_t low_surrogate_last = 0xDFFF;

// Units an extension can have for NumberedName() to keep it. A longer tail is
// not an extension, so the copy number cannot push the name past its limit.
constexpr std::size_t max_extension_units = 16;

// Reports whether character is a UTF-16 high surrogate.
bool IsHighSurrogate(wchar_t character) {
  return character >= high_surrogate_first && character <= high_surrogate_last;
}

// Reports whether character is a UTF-16 low surrogate.
bool IsLowSurrogate(wchar_t character) {
  return character >= low_surrogate_first && character <= low_surrogate_last;
}

// Cuts name to at most limit units, never between the halves of a surrogate
// pair.
void CutToUnits(std::wstring& name, std::size_t limit) {
  if (name.size() <= limit) {
    return;
  }
  // The cut falls between the two halves of a surrogate pair.
  const bool splits_pair = limit > 0 && IsHighSurrogate(name.at(limit - 1)) &&
                           IsLowSurrogate(name.at(limit));
  name.resize(splits_pair ? limit - 1 : limit);
}

// Drops the dots and spaces that end name, which Win32 never keeps.
void StripTrailingDotsAndSpaces(std::wstring& name) {
  const std::size_t last = name.find_last_not_of(L". ");
  name.resize(last == std::wstring::npos ? 0 : last + 1);
}

// Reports whether name is a device name once its extension is dropped. A
// trailing space before the dot does not hide it, and the case does not
// matter.
bool IsReservedDeviceName(std::wstring_view name) {
  std::wstring base(name.substr(0, name.find(L'.')));
  StripTrailingDotsAndSpaces(base);
  std::ranges::transform(base, base.begin(), [](wchar_t character) {
    return (character >= L'a' && character <= L'z')
               ? static_cast<wchar_t>(character - L'a' + L'A')
               : character;
  });

  if (std::ranges::find(reserved_device_names, base) !=
      reserved_device_names.end()) {
    return true;
  }
  constexpr std::size_t numbered_length = 4;
  if (base.size() != numbered_length) {
    return false;
  }
  const std::wstring_view stem = std::wstring_view(base).substr(0, 3);
  const wchar_t number = base.at(3);
  // A digit from 1 to 9, or a superscript one.
  const bool is_digit =
      (number >= L'1' && number <= L'9') ||
      std::ranges::find(superscript_digits, number) != superscript_digits.end();
  return is_digit && std::ranges::find(numbered_device_names, stem) !=
                         numbered_device_names.end();
}

// The separator the host puts between the parts of a path.
#ifdef _WIN32
constexpr wchar_t host_separator = L'\\';
#else
constexpr wchar_t host_separator = L'/';
#endif

// The path of the file called name in directory, with a separator between.
std::wstring JoinPath(std::wstring_view directory, std::wstring_view name) {
  std::wstring path(directory);
  if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
    path += host_separator;
  }
  path += name;
  return path;
}

// name with a copy number before its extension: "a.txt" becomes "a (2).txt".
// The stem gives up units when the result would pass kMaxFileNameUnits.
std::wstring NumberedName(const std::wstring& name, unsigned copy) {
  const std::size_t dot = name.rfind(L'.');
  // An extension is a short tail after a dot. A dot at the start makes a hidden
  // file's name, not an extension.
  const bool has_extension = dot != std::wstring::npos && dot > 0 &&
                             name.size() - dot <= max_extension_units;
  std::wstring stem = has_extension ? name.substr(0, dot) : name;
  const std::wstring extension = has_extension ? name.substr(dot) : L"";
  const std::wstring suffix = std::format(L" ({})", copy);

  CutToUnits(stem, kMaxFileNameUnits - suffix.size() - extension.size());
  return stem + suffix + extension;
}

#ifdef _WIN32

// Reports whether a failed CREATE_NEW of path says that the name is taken: by
// a file, or by a folder, or by a file the caller cannot open.
bool IsNameTaken(const std::wstring& path, DWORD error) {
  return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ||
         GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// What the host reported for a failed open: GetLastError() or errno.
using OpenError = DWORD;

// Opens path for writing. A new file is the only choice unless replace is set.
// Returns no_file_handle on failure, and the error in last_error. No sharing:
// nothing else may write the file while it is recovered.
NativeFileHandle OpenForRecovery(const std::wstring& path, bool replace,
                                 OpenError& last_error) {
  const DWORD creation = replace ? CREATE_ALWAYS : CREATE_NEW;
  HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                  creation, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    last_error = GetLastError();
    return no_file_handle;
  }
  return reinterpret_cast<NativeFileHandle>(handle);
}

// Reports whether the failure of OpenForRecovery() says the name is taken.
bool OpenFailedOnTakenName(const std::wstring& path, OpenError last_error) {
  return IsNameTaken(path, last_error);
}

// Reports whether the failure of OpenForRecovery() says the path is too long.
bool OpenFailedOnLength(OpenError last_error) {
  return last_error == ERROR_FILENAME_EXCED_RANGE;
}

#else

// What the host reported for a failed open: GetLastError() or errno.
using OpenError = int;

// The mode of a file a recovery creates, before the umask.
constexpr mode_t recovery_file_mode = 0644;

// Bytes of UTF-8 a recovered name keeps. A file system takes 255. The rest is
// the room for " (9999)".
constexpr std::size_t max_name_bytes = 240;

// Opens path for writing. A new file is the only choice unless replace is set.
// Returns no_file_handle on failure, and errno in last_error.
NativeFileHandle OpenForRecovery(const std::wstring& path, bool replace,
                                 OpenError& last_error) {
  const std::string narrow = Tool::DeviceAccess::PathFromWide(path).string();
  const int flags =
      O_WRONLY | O_CREAT | O_CLOEXEC | (replace ? O_TRUNC : O_EXCL);
  const int descriptor = open(narrow.c_str(), flags, recovery_file_mode);
  if (descriptor < 0) {
    last_error = errno;
    return no_file_handle;
  }
  return descriptor;
}

// Reports whether the failure of OpenForRecovery() says the name is taken.
bool OpenFailedOnTakenName(const std::wstring& /*path*/, OpenError last_error) {
  return last_error == EEXIST;
}

// Reports whether the failure of OpenForRecovery() says the path is too long.
bool OpenFailedOnLength(OpenError last_error) {
  return last_error == ENAMETOOLONG;
}

// Cuts name to at most max_name_bytes bytes of UTF-8, one character at a time.
void CutToUtf8Bytes(std::wstring& name) {
  while (Tool::DeviceAccess::WideToUtf8(name).size() > max_name_bytes) {
    name.pop_back();
  }
}

#endif

}  // namespace

std::wstring SafeFileName(std::wstring_view name) {
  std::wstring safe(name);
  for (wchar_t& character : safe) {
    if (character < first_printable ||
        forbidden_name_characters.find(character) != std::wstring_view::npos) {
      character = replacement_character;
    }
  }

  StripTrailingDotsAndSpaces(safe);
  if (safe.empty()) {
    safe = std::wstring(1, replacement_character);
  }
  if (IsReservedDeviceName(safe)) {
    safe.insert(safe.begin(), replacement_character);
  }

  CutToUnits(safe, kMaxFileNameUnits);
  // The cut can leave a dot or a space at the end.
  StripTrailingDotsAndSpaces(safe);
  if (safe.empty()) {
    safe = std::wstring(1, replacement_character);
  }
  return safe;
}

RecoveryFile::RecoveryFile(NativeFileHandle handle, std::wstring path,
                           NameChange change)
    : handle_(handle), path_(std::move(path)), change_(change) {}

RecoveryFile::RecoveryFile(RecoveryFile&& other) noexcept
    : handle_(std::exchange(other.handle_, no_file_handle)),
      path_(std::move(other.path_)),
      change_(other.change_) {}

RecoveryFile& RecoveryFile::operator=(RecoveryFile&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = std::exchange(other.handle_, no_file_handle);
    path_ = std::move(other.path_);
    change_ = other.change_;
  }
  return *this;
}

RecoveryFile::~RecoveryFile() { Close(); }

// Closes the file, once: a moved-from object has no handle.
void RecoveryFile::Close() noexcept {
  if (handle_ == no_file_handle) {
    return;
  }
#ifdef _WIN32
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
  close(static_cast<int>(handle_));
#endif
  handle_ = no_file_handle;
}

bool RecoveryFile::Write(std::span<const std::uint8_t> chunk) {
  if (handle_ == no_file_handle) {
    return false;
  }
#ifdef _WIN32
  DWORD written = 0;
  return WriteFile(reinterpret_cast<HANDLE>(handle_), chunk.data(),
                   gsl::narrow<DWORD>(chunk.size()), &written,
                   nullptr) != FALSE &&
         written == chunk.size();
#else
  while (!chunk.empty()) {
    const ssize_t written =
        ::write(static_cast<int>(handle_), chunk.data(), chunk.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    chunk = chunk.subspan(static_cast<std::size_t>(written));
  }
  return true;
#endif
}

const std::wstring& RecoveryFile::Path() const noexcept { return path_; }

NameChange RecoveryFile::Change() const noexcept { return change_; }

CreatedFile CreateRecoveryFile(std::wstring_view directory,
                               std::wstring_view name, ExistingFile existing) {
  std::wstring safe = SafeFileName(name);
#ifndef _WIN32
  CutToUtf8Bytes(safe);
#endif
  const NameChange first_change =
      safe == name ? NameChange::None : NameChange::Unsafe;
  const bool replace = existing == ExistingFile::Replace;

  for (unsigned copy = 1; copy <= kMaxNameCopies; copy++) {
    const std::wstring candidate = copy == 1 ? safe : NumberedName(safe, copy);
    const std::wstring path = JoinPath(directory, candidate);
    const std::wstring extended = Device::ExtendedPath(path);
    if (extended.size() >= kMaxExtendedPathUnits) {
      return {.file = std::nullopt, .error = CreateError::PathTooLong};
    }

    OpenError last_error = 0;
    const NativeFileHandle handle =
        OpenForRecovery(extended, replace, last_error);
    if (handle != no_file_handle) {
      return {.file = RecoveryFile(
                  handle, path, copy == 1 ? first_change : NameChange::Taken),
              .error = CreateError::Failed};
    }
    if (OpenFailedOnLength(last_error)) {
      return {.file = std::nullopt, .error = CreateError::PathTooLong};
    }
    if (replace || !OpenFailedOnTakenName(extended, last_error)) {
      return {.file = std::nullopt, .error = CreateError::Failed};
    }
  }
  return {.file = std::nullopt, .error = CreateError::NoFreeName};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel
