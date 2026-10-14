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

// Pump the message queue once per this many records. Keeps the Stop button
// responsive without a clock read per record.
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

}  // namespace

std::optional<SearchResult>
    SearchDeletedFiles(NtfsVolume<Cache::Strategy::FullCache>& volume,
                       std::wstring_view filter, const SearchControl& control) {
  const std::wstring pattern = NormalizeFilter(filter);
  std::wregex regx;
  try {
    regx = std::wregex(pattern);
  } catch (const std::regex_error&) {
    return std::nullopt;
  }

  SearchResult result;
  const auto begin = std::chrono::steady_clock::now();
  auto last_pump = begin;

  // Only parse Standard Information and File Name attributes
  // StdInfo will always be parsed
  Io::FileRecord fr(volume);

  std::map<ULONGLONG, ULONGLONG> id_to_parent;
  std::set<ULONGLONG> files;

  for (auto i = static_cast<ULONGLONG>(Mft::Idx::Mft);
       i < volume.GetRecordsCount(); i++) {
    if (i == control.max_records) {
      break;
    }
    if (control.stop_requested && control.stop_requested()) {
      break;
    }

    if (i % kPumpRecordInterval == 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_pump >= kPumpTimeInterval) {
        last_pump = now;
        if (control.pump_messages && !control.pump_messages()) {
          break;
        }
      }
    }

    fr.SetAttrMask(Attr::Mask::FileName | Attr::Mask::IndexRoot |
                   Attr::Mask::IndexAllocation);
    if (!fr.ParseFileRecord(i)) {
      continue;
    }
    if (!fr.ParseAttrs()) {
      continue;
    }

    // Check file name
    const std::wstring_view fn = fr.GetFileName();
    if (fn.empty()) {
      continue;
    }

    // Walked for id_to_parent, though not itself a candidate below.
    if (fr.IsDirectory()) {
      fr.TraverseSubEntries(
          [&fr, &id_to_parent](const IndexEntryView& ie, void* context) {
            if (ie.GetFileReference() == *fr.GetFileReference()) {
              return;
            }
            id_to_parent.insert(
                {ie.GetFileReference(), *fr.GetFileReference()});
          },
          nullptr);
    }

    // Only deleted records are candidates for recovery
    if (fr.IsDeleted()) {
      files.insert(*fr.GetFileReference());
    }
  }

  const auto end = std::chrono::steady_clock::now();
  result.scan_us =
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count();

  for (auto fri : files) {
    fr.SetAttrMask(Attr::Mask::FileName);
    if (!fr.ParseFileRecord(fri)) {
      continue;
    }
    if (!fr.ParseAttrs()) {
      continue;
    }
    const std::wstring_view fn = fr.GetFileName();

    const std::wstring sw = {fn.begin(), fn.end()};
    if (!std::regex_match(sw, regx)) {
      continue;
    }

    FILETIME ft{};
    fr.GetFileTime(&ft, nullptr, nullptr);

    // Full File name
    std::wstring full_file_name = sw;
    auto id = id_to_parent.find(fri);
    while (id != id_to_parent.end()) {
      fr.SetAttrMask(Attr::Mask::FileName);
      if (!fr.ParseFileRecord(id->second)) {
        break;
      }
      if (!fr.ParseAttrs()) {
        break;
      }
      std::wstring fn2 = std::wstring{fr.GetFileName()};
      full_file_name = fn2 + L"\\" + full_file_name;
      id = id_to_parent.find(id->second);
    }

    result.files.push_back({.reference = fri,
                            .name = sw,
                            .full_path = full_file_name,
                            .write_time = ToTicks(ft)});
    if (result.files.size() >= control.max_listed) {
      result.truncated = true;
      break;
    }
  }

  return result;
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
