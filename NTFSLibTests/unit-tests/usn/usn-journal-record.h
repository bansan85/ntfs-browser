#pragma once

#include <ntfs-browser/win-types.h>

#include <string>
#include <utility>

#include <ntfs-browser/namespace.h>

#include "fake-ntfs-image.h"

namespace NTFS_BROWSER_NAMESPACE::Tests::UsnSupport {

// The fields of a V2 journal record that the MftTree journal tests vary.
struct JournalRecordSpec {
  std::wstring name;
  ULONGLONG record;
  WORD sequence;
  DWORD reason;
  ULONGLONG time;
  // The docs directory of the MftTree fixture, unless a test names another.
  ULONGLONG parent{mft_tree_docs_idx};
  WORD parent_sequence{1};
};

// A V2 record of the file spec names, filed under its parent. The file is an
// archive file, as the fixtures' files are.
inline FakeUsnRecord MakeRecord(JournalRecordSpec spec) {
  return FakeUsnRecord{.name = std::move(spec.name),
                       .file_record = spec.record,
                       .file_sequence = spec.sequence,
                       .parent_record = spec.parent,
                       .parent_sequence = spec.parent_sequence,
                       .time = spec.time,
                       .reason = spec.reason,
                       .file_attributes = fake_usn_default_attributes};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::UsnSupport
