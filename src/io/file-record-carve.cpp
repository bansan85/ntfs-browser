#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <memory>
#include <span>
#include <tuple>
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
#include "attr/slot.h"
#include "attr/std-info.h"
#include "io/file-record-attr-types.h"
#include "io/file-record-impl.h"
#include "io/file-record-index.h"
#include "log/ntfs-common.h"
#include "record/slack-carver.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// The $STANDARD_INFORMATION wrapper of strategy S.
template <Cache::Strategy S>
using StdInfoType = Attr::AttrStdInfo<ResidentOf<S>, S>;

}  // namespace

// The carvers report UTC, as stored. The times of the file getters are local.
template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::ConvertCarvedTimes(
    std::vector<Record::CarvedName>& carved) {
  const auto to_local = [](FILETIME& time) {
    constexpr unsigned dword_bits = 32;
    const ULONGLONG utc =
        (static_cast<ULONGLONG>(time.dwHighDateTime) << dword_bits) |
        time.dwLowDateTime;
    StdInfoType<S>::UTC2Local(utc, time);
  };
  for (Record::CarvedName& name : carved) {
    // Unknown times stay zero.
    if (!name.times_known) {
      continue;
    }
    to_local(name.create_time);
    to_local(name.write_time);
    to_local(name.change_time);
    to_local(name.access_time);
  }
}

// Carves the bytes after the end marker, patched like the rest of the record,
// then drops the names this record also holds live.
template <Cache::Strategy S>
std::vector<Record::CarvedName>
    Io::FileRecord<S>::Impl::CarveSlackNames() const {
  const VolumeOptions& options = volume->GetOptions();
  if (!options.recover_errors || !options.include_deleted || !file_record ||
      !end_marker_offset) {
    return {};
  }

  const std::span<const BYTE> bytes(file_record->GetData()->raw,
                                    file_record->buffer_size);
  std::vector<Record::CarvedName> carved =
      Record::CarveFileNames(bytes, *end_marker_offset);

  const auto& live_names =
      std::get<Attr::AttrIndex(Attr::Type::FileName)>(attr_list);
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

  ConvertCarvedTimes(carved);

  if (!carved.empty()) {
    Log::Debug("File record {}: {} name(s) carved from the slack",
               file_reference.value_or(0), carved.size());
  }
  return carved;
}

// The names carved from the slack of the index blocks, with the times of the
// file getters.
template <Cache::Strategy S>
std::vector<Record::CarvedName>
    Io::FileRecord<S>::Impl::CarveIndexSlackNames() const {
  std::vector<Record::CarvedName> carved =
      DirectoryIndex<S>(*self).CarveSlackNames();
  ConvertCarvedTimes(carved);
  if (!carved.empty()) {
    Log::Debug("File record {}: {} name(s) carved from the index slack",
               file_reference.value_or(0), carved.size());
  }
  return carved;
}

template void FileRecord<Cache::Strategy::NoCache>::Impl::ConvertCarvedTimes(
    std::vector<Record::CarvedName>& carved);
template void FileRecord<Cache::Strategy::FullCache>::Impl::ConvertCarvedTimes(
    std::vector<Record::CarvedName>& carved);
template std::vector<Record::CarvedName>
    FileRecord<Cache::Strategy::NoCache>::Impl::CarveSlackNames() const;
template std::vector<Record::CarvedName>
    FileRecord<Cache::Strategy::FullCache>::Impl::CarveSlackNames() const;
template std::vector<Record::CarvedName>
    FileRecord<Cache::Strategy::NoCache>::Impl::CarveIndexSlackNames() const;
template std::vector<Record::CarvedName>
    FileRecord<Cache::Strategy::FullCache>::Impl::CarveIndexSlackNames() const;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
