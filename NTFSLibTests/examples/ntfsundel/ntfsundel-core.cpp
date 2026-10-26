#include "ntfsundel-core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <format>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>

#include <gsl/narrow>
#include <windows.h>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

using namespace NTFS_BROWSER_NAMESPACE;

namespace NtfsUndel {

namespace {

// Records between two pump checks. Keeps the modulo test the only per-record
// cost. kPumpTimeInterval caps how often the pump actually runs.
constexpr std::uint64_t kPumpRecordInterval = 5000;

// Minimum time between two pumps. Bounds worst-case Stop latency on a slow
// disk.
constexpr std::chrono::milliseconds kPumpTimeInterval(100);

// Filter that matches every name. Used when the user leaves the box empty.
constexpr std::wstring_view kMatchAllFilter = L".*";

// What the user is told when the data of a freed file probably belongs to
// another file now. Ends with the question the dialog asks.
constexpr std::wstring_view kReallocatedMessage =
    L"The clusters of this file were given to another file after it was "
    L"deleted. The recovered data probably belongs to that other file. "
    L"Continue anyway ?";

// What the user is told when clusters of a freed file are marked bad.
constexpr std::wstring_view kBadClustersMessage =
    L"Some clusters of this file are marked bad. The recovered data may be "
    L"incomplete, or the read may fail. Continue anyway ?";

// What the user is told when the allocation of a freed file could not be read.
// It is a warning, not a question.
constexpr std::wstring_view kUncheckedMessage =
    L"The allocation of the clusters of this file could not be checked. The "
    L"recovered data may be wrong.";

// Trims blanks off the filter. An empty filter becomes the match-all pattern.
std::wstring NormalizeFilter(std::wstring_view filter) {
  if (filter.empty()) {
    return std::wstring(kMatchAllFilter);
  }
  const auto is_blank = [](wchar_t c) { return std::iswspace(c) != 0; };
  const auto first = std::find_if_not(filter.begin(), filter.end(), is_blank);
  const auto last =
      std::find_if_not(filter.rbegin(), filter.rend(), is_blank).base();
  if (first >= last) {
    return {};
  }
  return std::wstring(first, last);
}

// Packs a FILETIME into one 64-bit tick count.
std::uint64_t ToTicks(const FILETIME& ft) {
  return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32U) |
         ft.dwLowDateTime;
}

// Record handle the search parses into, one per pass.
using FullRecord = Io::FileRecord<Cache::Strategy::FullCache>;

// Parent record number of every directory entry the scan saw, by child.
using ParentMap = std::map<ULONGLONG, ULONGLONG>;

// What the record pass collected for the listing pass.
struct ScanResult {
  ParentMap id_to_parent;
  std::set<ULONGLONG> freed;
};

// Parses record ref with only the attributes in mask. False if either step
// fails.
bool ParseWithMask(FullRecord& fr, ULONGLONG ref, Attr::Mask mask) {
  fr.SetAttrMask(mask);
  return fr.ParseFileRecord(ref) && fr.ParseAttrs();
}

// True once the record pass has reached its record limit or a stop request.
bool ScanMustEnd(const SearchControl& control, ULONGLONG record) {
  return record == control.max_records ||
         (control.stop_requested && control.stop_requested());
}

// True when the pass should pump messages now: on a record interval, and at
// most once per kPumpTimeInterval. Records the pump time when it returns true.
bool PumpDue(ULONGLONG record,
             std::chrono::steady_clock::time_point& last_pump) {
  if (record % kPumpRecordInterval != 0) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - last_pump < kPumpTimeInterval) {
    return false;
  }
  last_pump = now;
  return true;
}

// Records the parent of each entry fr's index lists, skipping its own
// self entry.
void RecordChildren(const FullRecord& fr, ParentMap& id_to_parent) {
  fr.TraverseSubEntries(
      [&fr, &id_to_parent](const IndexEntryView& ie, void* context) {
        if (ie.GetFileReference() == *fr.GetFileReference()) {
          return;
        }
        id_to_parent.insert({ie.GetFileReference(), *fr.GetFileReference()});
      },
      nullptr);
}

// Walks every record from the first user-visible one, and collects the
// directory parents and the freed records.
ScanResult ScanRecords(NtfsVolume<Cache::Strategy::FullCache>& volume,
                       const SearchControl& control,
                       std::chrono::steady_clock::time_point begin) {
  ScanResult scan;
  FullRecord fr(volume);
  auto last_pump = begin;

  // Only parse Standard Information and File Name attributes
  // StdInfo will always be parsed
  const Attr::Mask scan_mask = Attr::Mask::FileName | Attr::Mask::IndexRoot |
                               Attr::Mask::IndexAllocation;

  for (auto i = static_cast<ULONGLONG>(Mft::Idx::Mft);
       i < volume.GetRecordsCount(); i++) {
    if (ScanMustEnd(control, i)) {
      break;
    }
    if (PumpDue(i, last_pump) && control.pump_messages &&
        !control.pump_messages()) {
      break;
    }
    if (!ParseWithMask(fr, i, scan_mask) || fr.GetFileName().empty()) {
      continue;
    }

    // Directories also feed id_to_parent; a freed one stays a candidate.
    if (fr.IsDirectory()) {
      RecordChildren(fr, scan.id_to_parent);
    }

    // Only deleted records are candidates for recovery
    if (fr.IsDeleted()) {
      scan.freed.insert(*fr.GetFileReference());
    }
  }
  return scan;
}

// Path of record ref: its name at the end, each directory the scan linked
// above it before that. A walk that reaches the root starts with a separator.
// A name no directory entry links stays bare. Stops at the first parent that
// does not parse, and at the first parent already on the path: a corrupt
// index can name a loop, and the walk must still end.
std::wstring BuildFullPath(FullRecord& fr, const ParentMap& id_to_parent,
                           ULONGLONG ref, std::wstring name) {
  std::set<ULONGLONG> on_path{ref};
  auto id = id_to_parent.find(ref);
  while (id != id_to_parent.end()) {
    // The root's own name is not a component. On a real volume it is ".".
    if (id->second == static_cast<ULONGLONG>(Mft::Idx::Root)) {
      return L"\\" + name;
    }
    // Seen before on this walk: the entries form a loop.
    if (!on_path.insert(id->second).second) {
      break;
    }
    if (!ParseWithMask(fr, id->second, Attr::Mask::FileName)) {
      break;
    }
    name = std::wstring{fr.GetFileName()} + L"\\" + name;
    id = id_to_parent.find(id->second);
  }
  return name;
}

// The listing row of freed record ref, or nullopt when it does not parse or
// its name does not match pattern.
std::optional<DeletedFile> MatchDeleted(FullRecord& fr, ULONGLONG ref,
                                        const std::wregex& pattern,
                                        const ParentMap& id_to_parent) {
  if (!ParseWithMask(fr, ref, Attr::Mask::FileName)) {
    return std::nullopt;
  }
  const std::wstring name{fr.GetFileName()};
  if (!std::regex_match(name, pattern)) {
    return std::nullopt;
  }

  FILETIME ft{};
  fr.GetFileTime(&ft, nullptr, nullptr);
  return DeletedFile{.reference = ref,
                     .name = name,
                     .full_path = BuildFullPath(fr, id_to_parent, ref, name),
                     .write_time = ToTicks(ft)};
}

// The filter as a regex, or nullopt when it does not compile.
std::optional<std::wregex> CompileFilter(const std::wstring& pattern) {
  try {
    return std::wregex(pattern);
  } catch (const std::regex_error&) {
    return std::nullopt;
  }
}

}  // namespace

std::optional<SearchResult>
    SearchDeletedFiles(NtfsVolume<Cache::Strategy::FullCache>& volume,
                       std::wstring_view filter, const SearchControl& control) {
  const std::optional<std::wregex> pattern =
      CompileFilter(NormalizeFilter(filter));
  if (!pattern) {
    return std::nullopt;
  }

  SearchResult result;
  const auto begin = std::chrono::steady_clock::now();
  const ScanResult scan = ScanRecords(volume, control, begin);
  const auto end = std::chrono::steady_clock::now();
  result.scan_us =
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count();

  FullRecord fr(volume);
  for (const ULONGLONG ref : scan.freed) {
    std::optional<DeletedFile> match =
        MatchDeleted(fr, ref, *pattern, scan.id_to_parent);
    if (!match) {
      continue;
    }
    result.files.push_back(*std::move(match));
    if (result.files.size() >= control.max_listed) {
      result.truncated = true;
      break;
    }
  }
  return result;
}

std::wstring TooManyFilesMessage() {
  return std::format(L"Too many files found, only the first {} will be shown",
                     kMaxListedFiles);
}

std::wstring FormatWriteTime(std::uint64_t ticks) {
  const FILETIME ft{.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFU),
                    .dwHighDateTime = static_cast<DWORD>(ticks >> 32U)};
  SYSTEMTIME st{};
  if (FileTimeToSystemTime(&ft, &st) == FALSE) {
    st = SYSTEMTIME{};
  }
  return std::format(L"{:04}-{:02}-{:02}  {:02}:{:02}", st.wYear, st.wMonth,
                     st.wDay, st.wHour, st.wMinute);
}

OpenedRecord OpenDeletedFile(NtfsVolume<Cache::Strategy::NoCache>& volume,
                             std::uint64_t reference) {
  auto record =
      std::make_unique<Io::FileRecord<Cache::Strategy::NoCache>>(volume);
  if (!record->ParseFileRecord(reference)) {
    return {.record = nullptr, .error = RecoverError::RecordParse};
  }

  record->SetAttrMask(Attr::Mask::Data);
  if (!record->ParseAttrs()) {
    return {.record = nullptr, .error = RecoverError::AttributeParse};
  }
  return {.record = std::move(record), .error = RecoverError::None};
}

CopyResult CopyUnnamedData(
    const Io::FileRecord<Cache::Strategy::NoCache>& record,
    const std::function<bool(std::uint64_t size)>& confirm_large,
    const std::function<bool(std::span<const BYTE> chunk)>& write) {
  // Unnamed Data attribute contains the file data
  const Attr::AttrBase<Cache::Strategy::NoCache>* data = record.FindStream({});
  if (data == nullptr) {
    return {.error = RecoverError::NoDataStream};
  }

  const ULONGLONG datalen = data->GetDataSize();
  ULONGLONG remain = datalen;

  // Check files with huge size (maybe something is error)
  if (datalen > kConfirmFileSize && !confirm_large(datalen)) {
    return {.error = RecoverError::Declined};
  }

  std::vector<BYTE> vec(kCopyChunkSize, '\0');
  for (ULONGLONG i = 0; i < datalen; i += kCopyChunkSize) {
    const std::optional<ULONGLONG> len =
        data->ReadData(i, {vec.data(), kCopyChunkSize});
    if (!len || (*len != kCopyChunkSize && *len != remain)) {
      return {.error = RecoverError::Read};
    }
    if (!write({vec.data(), static_cast<std::size_t>(*len)})) {
      return {.error = RecoverError::Write};
    }
    remain -= *len;
  }
  return {.error = RecoverError::None, .bytes = datalen};
}

RecoveryCheck
    AssessRecovery(const Io::FileRecord<Cache::Strategy::NoCache>& record) {
  const std::optional<Alloc::DataState> state = record.GetDataState({});
  if (!state) {
    return {};
  }
  switch (*state) {
    case Alloc::DataState::Intact:
      return {};
    case Alloc::DataState::Reallocated:
      return {.advice = RecoveryAdvice::Confirm,
              .cause = RecoveryCause::Reallocated};
    case Alloc::DataState::Bad:
      return {.advice = RecoveryAdvice::Confirm,
              .cause = RecoveryCause::BadClusters};
    case Alloc::DataState::Unknown:
      break;
  }
  return {.advice = RecoveryAdvice::Warn, .cause = RecoveryCause::Unchecked};
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

bool ShouldRecover(const Io::FileRecord<Cache::Strategy::NoCache>& record,
                   const std::function<bool(const RecoveryCheck&)>& confirm) {
  const RecoveryCheck check = AssessRecovery(record);
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

// The path of the file called name in directory, with a separator between.
std::wstring JoinPath(std::wstring_view directory, std::wstring_view name) {
  std::wstring path(directory);
  if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
    path += L'\\';
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

// Reports whether a failed CREATE_NEW of path says that the name is taken: by
// a file, or by a folder, or by a file the caller cannot open.
bool IsNameTaken(const std::wstring& path, DWORD error) {
  return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ||
         GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

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

RecoveryFile::RecoveryFile(void* handle, std::wstring path, NameChange change)
    : handle_(handle), path_(std::move(path)), change_(change) {}

RecoveryFile::RecoveryFile(RecoveryFile&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      path_(std::move(other.path_)),
      change_(other.change_) {}

RecoveryFile& RecoveryFile::operator=(RecoveryFile&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = std::exchange(other.handle_, nullptr);
    path_ = std::move(other.path_);
    change_ = other.change_;
  }
  return *this;
}

RecoveryFile::~RecoveryFile() { Close(); }

// Closes the file, once: a moved-from object has no handle.
void RecoveryFile::Close() noexcept {
  if (handle_ != nullptr) {
    CloseHandle(handle_);
    handle_ = nullptr;
  }
}

bool RecoveryFile::Write(std::span<const BYTE> chunk) {
  DWORD written = 0;
  return handle_ != nullptr &&
         WriteFile(handle_, chunk.data(), gsl::narrow<DWORD>(chunk.size()),
                   &written, nullptr) != FALSE &&
         written == chunk.size();
}

const std::wstring& RecoveryFile::Path() const noexcept { return path_; }

NameChange RecoveryFile::Change() const noexcept { return change_; }

CreatedFile CreateRecoveryFile(std::wstring_view directory,
                               std::wstring_view name, ExistingFile existing) {
  const std::wstring safe = SafeFileName(name);
  const NameChange first_change =
      safe == name ? NameChange::None : NameChange::Unsafe;

  for (unsigned copy = 1; copy <= kMaxNameCopies; copy++) {
    const std::wstring candidate = copy == 1 ? safe : NumberedName(safe, copy);
    const std::wstring path = JoinPath(directory, candidate);
    const std::wstring extended = Io::ToExtendedLengthPath(path);
    if (extended.size() >= kMaxExtendedPathUnits) {
      return {.file = std::nullopt, .error = CreateError::PathTooLong};
    }

    // No sharing: nothing else may write the file while it is recovered.
    const DWORD creation =
        existing == ExistingFile::Replace ? CREATE_ALWAYS : CREATE_NEW;
    HANDLE handle =
        CreateFileW(extended.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      return {.file = RecoveryFile(
                  handle, path, copy == 1 ? first_change : NameChange::Taken),
              .error = CreateError::Failed};
    }
    if (existing == ExistingFile::Replace ||
        !IsNameTaken(extended, GetLastError())) {
      return {.file = std::nullopt, .error = CreateError::Failed};
    }
  }
  return {.file = std::nullopt, .error = CreateError::NoFreeName};
}

}  // namespace NtfsUndel
