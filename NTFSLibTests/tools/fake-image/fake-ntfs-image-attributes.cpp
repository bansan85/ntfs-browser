#include <cassert>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gsl/narrow>
#include <gsl/util>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "attr/flags.h"
#include "data/file-record-flag.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "ntfs-browser/attr/header-common.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// The compressed bytes of the corrupt LZNT1 chunk: a valid chunk header
// (0xB002, little-endian), then a flag byte, and a compressed word whose
// displacement (1) reaches before anything was decompressed.
constexpr BYTE corrupt_chunk_header_low = 0x02;

constexpr BYTE corrupt_chunk_header_high = 0xB0;

////////////////////////////////////////////////////////////////////////////
// NTFS compression fixtures (see fake-ntfs-image.h for what each builds)
////////////////////////////////////////////////////////////////////////////

// Encodes "runs" into NTFS' real, delta-LCN run-list format at "dataRun"
// (terminated by 0x00), and returns the byte count written.
DWORD EncodeDataRuns(std::span<BYTE> data_run,
                     const std::vector<FakeDataRun>& runs) {
  DWORD run_len = 0;
  ULONGLONG previous_lcn = 0;

  for (const FakeDataRun& run : runs) {
    if (run.lcn) {
      // A delta that fits a LONG keeps the 4-byte LCN field; a wider one
      // takes the 8-byte field, so an LCN past 2^32 stays representable.
      const LONGLONG delta =
          gsl::narrow<LONGLONG>(*run.lcn) - gsl::narrow<LONGLONG>(previous_lcn);
      if (std::in_range<LONG>(delta)) {
        gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
        gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(run.clusters);
        const LONG narrow_delta = gsl::narrow<LONG>(delta);
        std::memcpy(&gsl::at(data_run, run_len), &narrow_delta,
                    sizeof(narrow_delta));
        run_len += sizeof(narrow_delta);
      } else {
        gsl::at(data_run, run_len++) = run_header8_lcn_bytes;
        gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(run.clusters);
        std::memcpy(&gsl::at(data_run, run_len), &delta, sizeof(delta));
        run_len += sizeof(delta);
      }
      previous_lcn = *run.lcn;
    } else {
      gsl::at(data_run, run_len++) = 0x01;
      gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(run.clusters);
    }
  }

  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list
  return run_len;
}

// Clusters a run list maps, and how many of them are real, not sparse.
struct RunClusters {
  ULONGLONG total = 0;
  ULONGLONG real = 0;
};

// Counts the clusters runs maps.
RunClusters CountRunClusters(const std::vector<FakeDataRun>& runs) {
  RunClusters count;
  for (const FakeDataRun& run : runs) {
    count.total += run.clusters;
    if (run.lcn) {
      count.real += run.clusters;
    }
  }
  return count;
}

// Writes the run list of layout at the start of dest: the hand-encoded
// overrides.raw_runs when there are some, else the encoding of layout.runs.
// Returns the byte count written.
DWORD WriteRunList(std::span<BYTE> dest, const FakeNonResidentAttr& layout) {
  const std::vector<BYTE>& raw_runs = layout.overrides.raw_runs;
  if (raw_runs.empty()) {
    return EncodeDataRuns(dest, layout.runs);
  }
  std::memcpy(dest.data(), raw_runs.data(), raw_runs.size());
  return gsl::narrow<DWORD>(raw_runs.size());
}

}  // namespace

DWORD WriteNonResidentAttr(FakeRecord& record, DWORD offset, Attr::Type type,
                           const FakeNonResidentAttr& layout) {
  const FakeNonResidentOverrides& overrides = layout.overrides;
  auto& attr = InitNonResidentAttr(record, offset, type);
  const std::vector<WORD> encoded_name = ToUtf16(overrides.name);
  assert(encoded_name.size() <= 255 && "on-disk name_length is one byte");
  attr.header.name_length = gsl::narrow<BYTE>(encoded_name.size());
  attr.header.flags = overrides.flags.value_or(
      (layout.comp_unit_size != 0)
          ? NTFS_BROWSER_NAMESPACE::Attr::flag_compressed
          : static_cast<WORD>(0));

  const RunClusters clusters = CountRunClusters(layout.runs);
  attr.start_vcn = overrides.start_vcn;
  attr.last_vcn = overrides.last_vcn.value_or(
      overrides.start_vcn + ((clusters.total == 0) ? 0 : clusters.total - 1));
  attr.comp_unit_size = layout.comp_unit_size;
  attr.alloc_size = clusters.total * cluster_size;
  attr.real_size = layout.real_size;
  attr.ini_size = overrides.ini_size.value_or(layout.real_size);

  const auto header_size = gsl::narrow<WORD>(
      sizeof(attr) +
      ((layout.comp_unit_size != 0)
           ? NTFS_BROWSER_NAMESPACE::Data::compressed_size_field_size
           : 0));
  const size_t name_bytes = encoded_name.size() * sizeof(WORD);
  if (name_bytes != 0) {
    attr.header.name_offset = header_size;
    std::memcpy(&record.at(offset + header_size), encoded_name.data(),
                name_bytes);
  }
  const auto run_offset = gsl::narrow<WORD>(header_size + name_bytes);
  attr.data_run_offset = run_offset;

  if (layout.comp_unit_size != 0) {
    // CompressedSize: total allocated size of the attribute's compressed
    // clusters, i.e. everything actually on disk (the sparse padding of each
    // compressed unit excluded).
    const ULONGLONG compressed_size = clusters.real * cluster_size;
    std::memcpy(&record.at(offset + sizeof(attr)), &compressed_size,
                sizeof(compressed_size));
  }

  const DWORD run_len = WriteRunList(
      std::span<BYTE>(record).subspan(offset + run_offset), layout);
  attr.header.total_size =
      overrides.total_size.value_or(AlignAttrSize(run_offset + run_len));
  return attr.header.total_size;
}

// File record (root, #5): resident $STANDARD_INFORMATION("permission") plus
// one non-resident $DATA attribute described by compUnitSize/realSize/runs.
FakeRecord MakeNonResidentDataRecord(
    NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission permission,
    WORD comp_unit_size, ULONGLONG real_size,
    const std::vector<FakeDataRun>& runs,
    const FakeNonResidentOverrides& overrides) {
  FakeRecord record = MakeInUseFileRecord();

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(record, offset, permission);
  offset += WriteNonResidentAttr(record, offset, Attr::Type::Data,
                                 {.comp_unit_size = comp_unit_size,
                                  .real_size = real_size,
                                  .runs = runs,
                                  .overrides = overrides});

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Directory record (root, #5): resident $STANDARD_INFORMATION("permission"),
// a resident $INDEX_ROOT holding "rootNames" as leaf entries followed by a
// nameless SUBNODE-only entry, and a non-resident $INDEX_ALLOCATION - lets
// fixtures target an attribute FuzzOnce() actually ReadData()s, unlike plain
// $DATA.
FakeRecord MakeIndexAllocationDirRecord(
    NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission permission,
    const FakeNonResidentAttr& index_alloc,
    std::span<const FakeIndexName> root_names) {
  FakeRecord record = MakeInUseDirectoryRecord();

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(record, offset, permission);

  offset += WriteIndexRootAttr(record, offset, root_names);
  offset += WriteNonResidentAttr(record, offset, Attr::Type::IndexAllocation,
                                 index_alloc);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Lays "clusterBytes" down over the real runs of "runs", in run order, growing
// "image" to hold them; sparse runs consume no bytes and stay zero-filled.
void LayRunBytes(std::vector<BYTE>& image, const std::vector<FakeDataRun>& runs,
                 const std::vector<BYTE>& cluster_bytes) {
  // Grow the image so every real run fits, rounded up to FullCache's whole
  // 64KiB read block.
  size_t highest_end = image.size();
  for (const FakeDataRun& run : runs) {
    if (run.lcn) {
      const size_t end = (static_cast<size_t>(*run.lcn) + run.clusters) *
                         static_cast<size_t>(cluster_size);
      highest_end = (end > highest_end) ? end : highest_end;
    }
  }
  PadToFullCacheRead(image, highest_end);

  size_t written = 0;
  for (const FakeDataRun& run : runs) {
    if (!run.lcn || written >= cluster_bytes.size()) {
      continue;
    }

    const size_t capacity =
        static_cast<size_t>(run.clusters) * static_cast<size_t>(cluster_size);
    const size_t left = cluster_bytes.size() - written;
    const size_t chunk = (left < capacity) ? left : capacity;
    std::memcpy(&image.at(static_cast<size_t>(*run.lcn) *
                          static_cast<size_t>(cluster_size)),
                &cluster_bytes.at(written), chunk);
    written += chunk;
  }
}

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// "record" and "clusterBytes" laid down over its real runs, in run order;
// sparse runs consume no bytes and stay zero-filled.
std::vector<BYTE>
    BuildCompressionImage(const FakeRecord& record,
                          const std::vector<FakeDataRun>& runs,
                          const std::vector<BYTE>& cluster_bytes) {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  PutRootRecord(image, record);

  LayRunBytes(image, runs, cluster_bytes);
  return image;
}

// The malformed LZNT1 bytes both corrupt-compression fixtures use: a
// well-formed compressed chunk header, then a compressed word whose
// displacement (1) reaches before anything has been decompressed yet.
std::vector<BYTE> MakeCorruptLznt1Chunk() {
  return {corrupt_chunk_header_low, corrupt_chunk_header_high, 0x01, 0x00,
          0x00};
}

// The 1024-byte "INDX"-signed index block a compressed $INDEX_ALLOCATION
// decompresses to: "names" as leaf FILE_NAME entries plus the terminating
// entry. Built standalone since it is the *decompressed* content, wrapped
// into an LZNT1 chunk by the caller.
std::vector<BYTE> MakeIndexBlockContent(std::span<const FakeIndexName> names) {
  std::vector<BYTE> content(fake_file_record_size, 0);

  const std::span<BYTE> block_start = content;
  auto& block = WriteIndexBlockHeader(block_start, fake_file_record_size, 0);
  const std::span<BYTE> body =
      block_start.subspan(sizeof(NTFS_BROWSER_NAMESPACE::Data::IndexBlock));

  DWORD entry_bytes = 0;
  for (const FakeIndexName& name : names) {
    entry_bytes += WriteFilenameEntry(&gsl::at(body, entry_bytes), name);
  }
  entry_bytes += WriteTerminalEntry(&gsl::at(body, entry_bytes));

  block.total_entry_size = entry_bytes;
  block.alloc_entry_size = entry_bytes;

  return content;
}

// The single "Comp" entry every compressed $INDEX_ALLOCATION fixture but
// the surrogate-pair one decompresses to.
std::vector<BYTE> MakeCompressedIndexBlockContent() {
  const FakeIndexName comp{.name = compressed_index_entry_name,
                           .mft_ref = compressed_index_entry_mft_ref,
                           .directory = false};
  return MakeIndexBlockContent(std::span(&comp, 1));
}

// Writes a malformed attribute at record[offset], as defect describes it.
void WriteTrailingDefect(FakeRecord& record, DWORD offset,
                         FakeTrailingDefect defect) {
  // A resident header never fits in this: ParseAttrs() rejects it.
  constexpr DWORD undersized_total_size = 8;

  switch (defect) {
    case FakeTrailingDefect::UndersizedHeader: {
      auto& attr =
          *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderResident*>(
              &record.at(offset));
      attr.header.type = Attr::Type::Bitmap;
      attr.header.non_resident = 0;
      attr.header.total_size = undersized_total_size;
      break;
    }
    case FakeTrailingDefect::UndersizedCompressedField: {
      auto& attr =
          *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
              &record.at(offset));
      attr.header.type = Attr::Type::Bitmap;
      attr.header.non_resident = 1;
      attr.comp_unit_size = compression_unit_size_shift;
      attr.header.total_size =
          NTFS_BROWSER_NAMESPACE::Data::header_non_resident_base_size;
      break;
    }
    case FakeTrailingDefect::RejectedAttribute: {
      auto& attr =
          *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
              &record.at(offset));
      attr.header.type = Attr::Type::StandardInformation;
      attr.header.non_resident = 1;
      attr.header.total_size = AlignAttrSize(
          NTFS_BROWSER_NAMESPACE::Data::header_non_resident_base_size +
          run_list_room);
      break;
    }
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
