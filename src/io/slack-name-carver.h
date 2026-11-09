#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "io/file-record.h"
#include "record/carved-name.h"
#include "record/header.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Io {

// Recovers the names of deleted files that a directory or a file record still
// holds in its slack, and gives their times as the getters of the file report
// them. Short-lived: built over a parsed FileRecord for one carve.
template <Cache::Strategy S>
class SlackNameCarver {
 public:
  // record MUST outlive this object. file_reference is the reference the
  // record was parsed from, empty when it has none. It only names the record in
  // the log.
  SlackNameCarver(const FileRecord<S>& record,
                  std::optional<ULONGLONG> file_reference) noexcept;

  // The names in the bytes after the end marker of the attribute walk, patched
  // like the rest of the record, without those the record also holds live.
  // Empty unless the volume recovers and lists deleted items, and the walk of
  // header met an end marker at end_marker_offset.
  [[nodiscard]] std::vector<Record::CarvedName>
      FromRecord(const Record::HeaderImpl<S>* header,
                 std::optional<size_t> end_marker_offset) const;

  // The names in the slack of the index blocks of the record.
  [[nodiscard]] std::vector<Record::CarvedName> FromIndex() const;

 private:
  // The carvers report UTC, as stored. The times of the file getters are local.
  // Unknown times stay zero.
  static void ConvertTimes(std::vector<Record::CarvedName>& carved);

  const FileRecord<S>& record_;
  std::optional<ULONGLONG> file_reference_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
