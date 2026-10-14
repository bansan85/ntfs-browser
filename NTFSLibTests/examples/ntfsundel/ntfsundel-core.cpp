#include "ntfsundel-core.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <format>
#include <map>
#include <regex>
#include <set>

#include <windows.h>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>

using namespace NtfsBrowser;

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
// does not parse.
std::wstring BuildFullPath(FullRecord& fr, const ParentMap& id_to_parent,
                           ULONGLONG ref, std::wstring name) {
  auto id = id_to_parent.find(ref);
  while (id != id_to_parent.end()) {
    // The root's own name is not a component. On a real volume it is ".".
    if (id->second == static_cast<ULONGLONG>(Mft::Idx::Root)) {
      return L"\\" + name;
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

}  // namespace NtfsUndel
