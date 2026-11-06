#include "io/slack-name-carver.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/record/carved-name.h>
#include <ntfs-browser/volume-options.h>

#include "attr/file-name.h"
#include "attr/std-info.h"
#include "io/file-record-attr-types.h"
#include "io/file-record-index.h"
#include "log/ntfs-common.h"
#include "record/header.h"
#include "record/slack-carver.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

template <Cache::Strategy S>
SlackNameCarver<S>::SlackNameCarver(
    const FileRecord<S>& record,
    std::optional<ULONGLONG> file_reference) noexcept
    : record_(record), file_reference_(file_reference) {}

template <Cache::Strategy S>
void SlackNameCarver<S>::ConvertTimes(std::vector<Record::CarvedName>& carved) {
  const auto to_local = [](FILETIME& time) {
    constexpr unsigned dword_bits = 32;
    const ULONGLONG utc =
        (static_cast<ULONGLONG>(time.dwHighDateTime) << dword_bits) |
        time.dwLowDateTime;
    Attr::AttrStdInfo<ResidentOf<S>, S>::UTC2Local(utc, time);
  };
  for (Record::CarvedName& name : carved) {
    if (!name.times_known) {
      continue;
    }
    to_local(name.create_time);
    to_local(name.write_time);
    to_local(name.change_time);
    to_local(name.access_time);
  }
}

template <Cache::Strategy S>
std::vector<Record::CarvedName> SlackNameCarver<S>::FromRecord(
    const Record::HeaderImpl<S>* header,
    std::optional<size_t> end_marker_offset) const {
  const VolumeOptions& options = record_.GetVolume().GetOptions();
  if (!options.recover_errors || !options.include_deleted ||
      header == nullptr || !end_marker_offset) {
    return {};
  }

  const std::span<const BYTE> bytes(header->GetData()->raw,
                                    header->buffer_size);
  std::vector<Record::CarvedName> carved =
      Record::CarveFileNames(bytes, *end_marker_offset);

  const auto& live_names = record_.GetAttr(Attr::Type::FileName);
  std::erase_if(carved, [&live_names](const Record::CarvedName& candidate) {
    return std::ranges::any_of(
        live_names, [&candidate](const std::unique_ptr<Attr::AttrBase<S>>& fn) {
          const Filename& live =
              *static_cast<const Attr::AttrFileName<ResidentOf<S>, S>*>(
                  fn.get());
          return live.GetFilename() == candidate.name &&
                 (!candidate.parent_known ||
                  live.GetParentReference() == candidate.parent_record);
        });
  });

  ConvertTimes(carved);

  if (!carved.empty()) {
    Log::Debug("File record {}: {} name(s) carved from the slack",
               file_reference_.value_or(0), carved.size());
  }
  return carved;
}

template <Cache::Strategy S>
std::vector<Record::CarvedName> SlackNameCarver<S>::FromIndex() const {
  std::vector<Record::CarvedName> carved =
      DirectoryIndex<S>(record_).CarveSlackNames();
  ConvertTimes(carved);
  if (!carved.empty()) {
    Log::Debug("File record {}: {} name(s) carved from the index slack",
               file_reference_.value_or(0), carved.size());
  }
  return carved;
}

template class SlackNameCarver<Cache::Strategy::NoCache>;
template class SlackNameCarver<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
