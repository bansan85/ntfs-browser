#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/usn-record.h"
#include "fake-ntfs-image-internal.h"
#include "file-record-header-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;
using NTFS_BROWSER_NAMESPACE::Data::UsnFileId128;
using NTFS_BROWSER_NAMESPACE::Data::UsnRecordV2;
using NTFS_BROWSER_NAMESPACE::Data::UsnRecordV3;
using NTFS_BROWSER_NAMESPACE::Data::UsnRecordV4;

// Attribute flag of a sparse stream. The library names no such constant, since
// it reads sparseness from the runs.
constexpr WORD flag_sparse = 0x8000;

// Name of the stream that holds the journal.
constexpr std::wstring_view journal_stream_name = L"$J";

// Name of the journal's file.
constexpr std::wstring_view journal_file_name = L"$UsnJrnl";

// Sequence number $Extend carries, as the system records of a real volume do:
// their own record number.
constexpr WORD extend_sequence = 11;

// Sequence number of $UsnJrnl, and of the entry that names it.
constexpr WORD journal_sequence = 1;

// Byte that ends a run list early: its length field claims 15 bytes, which no
// run can have, so a decoder stops there.
constexpr BYTE malformed_run_header = 0x1F;

// First cluster of the journal's data when the data is not to be read: far
// past any fixture image, yet a cluster number a run can encode.
constexpr ULONGLONG unreadable_lcn = ULONGLONG{1} << 30U;

// Bytes of USN_RECORD_V4 as a fixture writes it: the fixed part only, with no
// extent.
constexpr DWORD v4_record_size = sizeof(UsnRecordV4);

// How many bytes of the length or the offset field a run header names, at most.
constexpr unsigned max_run_field_bytes = 8;

// The 1 KiB fake record holds the run list in what the header leaves: a hole, a
// run, the terminator. Far below that, so a fixture never overflows it.
constexpr size_t max_encoded_runs = 8;

// Smallest count of bytes that holds value as an unsigned number.
BYTE UnsignedByteCount(ULONGLONG value) {
  BYTE count = 1;
  while (count < max_run_field_bytes &&
         (value >> (bits_per_byte * count)) != 0) {
    ++count;
  }
  return count;
}

// Smallest count of bytes that holds value as a two's complement number.
BYTE SignedByteCount(LONGLONG value) {
  BYTE count = 1;
  while (count < max_run_field_bytes) {
    const unsigned bits = bits_per_byte * count;
    const LONGLONG limit = LONGLONG{1} << (bits - 1);
    if (value >= -limit && value < limit) {
      break;
    }
    ++count;
  }
  return count;
}

// Appends the low count bytes of value, least significant first.
void AppendLittleEndian(std::vector<BYTE>& out, ULONGLONG value, BYTE count) {
  for (BYTE i = 0; i < count; ++i) {
    out.push_back(
        static_cast<BYTE>((value >> (bits_per_byte * i)) & byte_mask));
  }
}

// Appends one run of the real run-list format: a hole when lcn_delta is empty,
// else a real run whose LCN is the previous one plus lcn_delta.
void AppendRun(std::vector<BYTE>& out, ULONGLONG clusters,
               std::optional<LONGLONG> lcn_delta) {
  const BYTE length_bytes = UnsignedByteCount(clusters);
  const BYTE offset_bytes = lcn_delta ? SignedByteCount(*lcn_delta) : 0;
  out.push_back(static_cast<BYTE>((offset_bytes << 4U) | length_bytes));
  AppendLittleEndian(out, clusters, length_bytes);
  if (lcn_delta) {
    AppendLittleEndian(out, static_cast<ULONGLONG>(*lcn_delta), offset_bytes);
  }
}

// One real stretch of $J, with its bytes.
struct Stretch {
  ULONGLONG start{0};
  std::vector<BYTE> bytes;
};

// Rounds up to a whole number of clusters.
ULONGLONG RoundUpToCluster(ULONGLONG bytes) {
  return (bytes + cluster_size - 1) / cluster_size * cluster_size;
}

// The bytes of records laid back to back from stream_offset, padded with zeros
// to a whole number of clusters.
std::vector<BYTE> EncodeRecords(const std::vector<FakeUsnRecord>& records,
                                ULONGLONG stream_offset) {
  std::vector<BYTE> bytes;
  for (const FakeUsnRecord& record : records) {
    const std::vector<BYTE> one =
        EncodeUsnRecord(record, stream_offset + bytes.size());
    bytes.insert(bytes.end(), one.begin(), one.end());
  }
  bytes.resize(RoundUpToCluster(bytes.size()), 0);
  return bytes;
}

// The real stretches of $J, in order, with the patches applied.
std::vector<Stretch> LayOutStretches(const FakeUsnOptions& options) {
  assert(options.sparse_head_bytes % cluster_size == 0);
  std::vector<Stretch> stretches;

  Stretch first{.start = options.sparse_head_bytes};
  if (!options.raw_stream.empty()) {
    first.bytes = options.raw_stream;
    first.bytes.resize(RoundUpToCluster(first.bytes.size()), 0);
  } else {
    first.bytes = EncodeRecords(options.records, first.start);
  }
  ULONGLONG cursor = first.start + first.bytes.size();
  if (!first.bytes.empty()) {
    stretches.push_back(std::move(first));
  }

  for (const FakeUsnStretch& more : options.more_stretches) {
    assert(more.hole_bytes % cluster_size == 0);
    Stretch next{.start = cursor + more.hole_bytes};
    next.bytes = EncodeRecords(more.records, next.start);
    cursor = next.start + next.bytes.size();
    if (!next.bytes.empty()) {
      stretches.push_back(std::move(next));
    }
  }

  for (const FakeUsnPatch& patch : options.patches) {
    bool applied = false;
    for (Stretch& stretch : stretches) {
      if (patch.stream_offset >= stretch.start &&
          patch.stream_offset + patch.bytes.size() <=
              stretch.start + stretch.bytes.size()) {
        std::ranges::copy(patch.bytes,
                          stretch.bytes.begin() +
                              gsl::narrow<std::ptrdiff_t>(patch.stream_offset -
                                                          stretch.start));
        applied = true;
        break;
      }
    }
    assert(applied && "a patch of $J must fall inside a real stretch");
    (void)applied;
  }
  return stretches;
}

// Where $J's data lives: its runs for the allocated-size accounting, the same
// runs as the real-run list format encodes them, and the real clusters' bytes.
struct JournalLayout {
  std::vector<FakeDataRun> runs;
  std::vector<BYTE> raw_runs;
  std::vector<BYTE> cluster_bytes;
  ULONGLONG stream_size{0};
};

// Lays $J out: a hole before each stretch, then the stretch's clusters one
// after the other starting at first_lcn.
JournalLayout MakeJournalLayout(const std::vector<Stretch>& stretches,
                                const FakeUsnOptions& options,
                                ULONGLONG first_lcn) {
  JournalLayout layout;
  ULONGLONG position = 0;
  ULONGLONG lcn = first_lcn;
  ULONGLONG previous_lcn = 0;
  bool first_real_run_done = false;

  const auto add_hole = [&](ULONGLONG hole_bytes) {
    if (hole_bytes == 0) {
      return;
    }
    const ULONGLONG clusters = hole_bytes / cluster_size;
    layout.runs.push_back(
        {.lcn = std::nullopt, .clusters = gsl::narrow<DWORD>(clusters)});
    if (!(options.cut_run_list && first_real_run_done)) {
      AppendRun(layout.raw_runs, clusters, std::nullopt);
    }
  };

  for (const Stretch& stretch : stretches) {
    add_hole(stretch.start - position);
    const ULONGLONG clusters = stretch.bytes.size() / cluster_size;
    layout.runs.push_back(
        {.lcn = lcn, .clusters = gsl::narrow<DWORD>(clusters)});
    if (!(options.cut_run_list && first_real_run_done)) {
      AppendRun(layout.raw_runs, clusters,
                gsl::narrow<LONGLONG>(lcn) -
                    gsl::narrow<LONGLONG>(previous_lcn));
    }
    first_real_run_done = true;
    layout.cluster_bytes.insert(layout.cluster_bytes.end(),
                                stretch.bytes.begin(), stretch.bytes.end());
    previous_lcn = lcn;
    lcn += clusters;
    position = stretch.start + stretch.bytes.size();
  }
  layout.stream_size = stretches.empty() ? options.sparse_head_bytes : position;
  if (stretches.empty()) {
    add_hole(options.sparse_head_bytes);
  }

  if (options.cut_run_list) {
    layout.raw_runs.push_back(malformed_run_header);
  }
  layout.raw_runs.push_back(0x00);
  assert(layout.runs.size() <= max_encoded_runs);
  return layout;
}

// Record 11: $Extend, in use, a directory whose index names $UsnJrnl unless the
// options leave it out.
FakeRecord MakeExtendRecord(const FakeUsnOptions& options) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, FileRecordFlag::InUse | FileRecordFlag::Dir);
  EditFileRecordHeader(record, [](FileRecordHeader& header) {
    header.seq_no = extend_sequence;
  });

  const ULONGLONG root = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Root), root_sequence_number);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(record, offset,
                              {.name = L"$Extend", .parent_ref = root}, true);

  const FakeIndexName journal_entry{.name = journal_file_name,
                                    .mft_ref = usn_journal_idx,
                                    .directory = false,
                                    .sequence = journal_sequence};
  if (options.omit_extend_entry) {
    offset += WriteIndexRootAttr(record, offset, {});
  } else {
    offset += WriteIndexRootAttr(
        record, offset, std::span<const FakeIndexName>(&journal_entry, 1));
  }
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// $UsnJrnl's record: its name, and the stream $J laid out as layout says.
FakeRecord MakeJournalRecord(const FakeUsnOptions& options,
                             const JournalLayout& layout) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value,
      options.journal_record_freed ? FileRecordFlag{} : FileRecordFlag::InUse);
  EditFileRecordHeader(record, [](FileRecordHeader& header) {
    header.seq_no = journal_sequence;
  });

  const ULONGLONG extend = MakeFileReference(
      static_cast<ULONGLONG>(Mft::Idx::Extend), extend_sequence);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(
      record, offset, {.name = journal_file_name, .parent_ref = extend}, false);
  if (options.omit_j_stream) {
    offset += WriteResidentDataAttr(record, offset, 0);
  } else {
    offset += WriteNonResidentAttr(
        record, offset, Attr::Type::Data,
        {.comp_unit_size = 0,
         .real_size = options.forged_size.value_or(layout.stream_size),
         .runs = layout.runs,
         .overrides = {.name = journal_stream_name,
                       .flags = flag_sparse,
                       .raw_runs = layout.raw_runs}});
  }
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Smallest multiple of 8 that holds size.
DWORD PaddedSize(size_t size) { return AlignAttrSize(size); }

// Bytes of a V2 record.
std::vector<BYTE> EncodeV2(const FakeUsnRecord& record, ULONGLONG offset) {
  const std::vector<WORD> name = ToUtf16(record.name);
  const size_t name_bytes = name.size() * sizeof(WORD);
  const DWORD total = PaddedSize(sizeof(UsnRecordV2) + name_bytes);

  UsnRecordV2 header{};
  header.common = {
      .record_length = total, .major_version = 2, .minor_version = 0};
  header.file_reference =
      MakeFileReference(record.file_record, record.file_sequence);
  header.parent_reference =
      MakeFileReference(record.parent_record, record.parent_sequence);
  header.usn = record.usn.value_or(gsl::narrow<LONGLONG>(offset));
  header.timestamp = record.time;
  header.reason = record.reason;
  header.file_attributes = record.file_attributes;
  header.file_name_length = gsl::narrow<WORD>(name_bytes);
  header.file_name_offset = sizeof(UsnRecordV2);

  std::vector<BYTE> bytes(total, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  if (name_bytes != 0) {
    std::memcpy(&bytes.at(sizeof(header)), name.data(), name_bytes);
  }
  return bytes;
}

// Bytes of a V3 record.
std::vector<BYTE> EncodeV3(const FakeUsnRecord& record, ULONGLONG offset) {
  const std::vector<WORD> name = ToUtf16(record.name);
  const size_t name_bytes = name.size() * sizeof(WORD);
  const DWORD total = PaddedSize(sizeof(UsnRecordV3) + name_bytes);

  UsnRecordV3 header{};
  header.common = {
      .record_length = total, .major_version = 3, .minor_version = 0};
  header.file_reference = UsnFileId128{
      .low = MakeFileReference(record.file_record, record.file_sequence),
      .high = record.file_reference_high};
  header.parent_reference = UsnFileId128{
      .low = MakeFileReference(record.parent_record, record.parent_sequence),
      .high = record.parent_reference_high};
  header.usn = record.usn.value_or(gsl::narrow<LONGLONG>(offset));
  header.timestamp = record.time;
  header.reason = record.reason;
  header.file_attributes = record.file_attributes;
  header.file_name_length = gsl::narrow<WORD>(name_bytes);
  header.file_name_offset = sizeof(UsnRecordV3);

  std::vector<BYTE> bytes(total, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  if (name_bytes != 0) {
    std::memcpy(&bytes.at(sizeof(header)), name.data(), name_bytes);
  }
  return bytes;
}

// Bytes of a V4 record: no name, no extent.
std::vector<BYTE> EncodeV4(const FakeUsnRecord& record, ULONGLONG offset) {
  UsnRecordV4 header{};
  header.common = {
      .record_length = v4_record_size, .major_version = 4, .minor_version = 0};
  header.file_reference = UsnFileId128{
      .low = MakeFileReference(record.file_record, record.file_sequence),
      .high = record.file_reference_high};
  header.parent_reference = UsnFileId128{
      .low = MakeFileReference(record.parent_record, record.parent_sequence),
      .high = record.parent_reference_high};
  header.usn = record.usn.value_or(gsl::narrow<LONGLONG>(offset));
  header.reason = record.reason;

  std::vector<BYTE> bytes(v4_record_size, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  return bytes;
}

}  // namespace

std::vector<BYTE> EncodeUsnRecord(const FakeUsnRecord& record,
                                  ULONGLONG stream_offset) {
  switch (record.major_version) {
    case 3:
      return EncodeV3(record, stream_offset);
    case 4:
      return EncodeV4(record, stream_offset);
    default:
      return EncodeV2(record, stream_offset);
  }
}

void PutUsnJournal(std::vector<BYTE>& image, const FakeUsnOptions& options) {
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  const std::vector<Stretch> stretches = LayOutStretches(options);
  const ULONGLONG first_lcn =
      options.unreadable_data
          ? unreadable_lcn
          : (image.size() + cluster_size - 1) / cluster_size;
  const JournalLayout layout = MakeJournalLayout(stretches, options, first_lcn);

  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Extend),
               MakeExtendRecord(options));
  if (!options.omit_journal_record) {
    PutMftRecord(image, mft_addr, usn_journal_idx,
                 MakeJournalRecord(options, layout));
  }

  if (!options.unreadable_data && !options.omit_j_stream) {
    LayRunBytes(image, layout.runs, layout.cluster_bytes);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
