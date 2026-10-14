#include <cassert>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/mft/idx.h>

#include "attr/flags.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-entry-flag.h"
#include "data/index-root.h"
#include "data/standard-information.h"
#include "fake-ntfs-image-internal.h"

namespace NtfsBrowserTests {

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

}  // namespace

DWORD WriteNonResidentAttr(FakeRecord& record, DWORD offset, Attr::Type type,
                           const FakeNonResidentAttr& layout) {
  const FakeNonResidentOverrides& overrides = layout.overrides;
  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
      &record.at(offset));
  attr.header.type = type;
  attr.header.non_resident = 1;
  const std::vector<WORD> encoded_name = ToUtf16(overrides.name);
  assert(encoded_name.size() <= 255 && "on-disk name_length is one byte");
  attr.header.name_length = gsl::narrow<BYTE>(encoded_name.size());
  attr.header.flags = overrides.flags.value_or(
      (layout.comp_unit_size != 0) ? NtfsBrowser::Attr::flag_compressed
                                   : static_cast<WORD>(0));
  attr.header.id = 0;

  ULONGLONG total_clusters = 0;
  ULONGLONG real_clusters = 0;
  for (const FakeDataRun& run : layout.runs) {
    total_clusters += run.clusters;
    if (run.lcn) {
      real_clusters += run.clusters;
    }
  }

  attr.start_vcn = overrides.start_vcn;
  attr.last_vcn = overrides.last_vcn.value_or(
      overrides.start_vcn + ((total_clusters == 0) ? 0 : total_clusters - 1));
  attr.comp_unit_size = layout.comp_unit_size;
  attr.alloc_size = total_clusters * cluster_size;
  attr.real_size = layout.real_size;
  attr.ini_size = overrides.ini_size.value_or(layout.real_size);

  const auto header_size = gsl::narrow<WORD>(
      sizeof(attr) + ((layout.comp_unit_size != 0)
                          ? NtfsBrowser::Data::compressed_size_field_size
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
    const ULONGLONG compressed_size = real_clusters * cluster_size;
    std::memcpy(&record.at(offset + sizeof(attr)), &compressed_size,
                sizeof(compressed_size));
  }

  DWORD run_len = 0;
  if (overrides.raw_runs.empty()) {
    run_len = EncodeDataRuns(
        std::span<BYTE>(record).subspan(offset + run_offset), layout.runs);
  } else {
    run_len = gsl::narrow<DWORD>(overrides.raw_runs.size());
    std::memcpy(&record.at(offset + run_offset), overrides.raw_runs.data(),
                run_len);
  }
  attr.header.total_size =
      overrides.total_size.value_or(AlignAttrSize(run_offset + run_len));
  return attr.header.total_size;
}

// File record (root, #5): resident $STANDARD_INFORMATION("permission") plus
// one non-resident $DATA attribute described by compUnitSize/realSize/runs.
FakeRecord
    MakeNonResidentDataRecord(NtfsBrowser::Data::StdInfoPermission permission,
                              WORD comp_unit_size, ULONGLONG real_size,
                              const std::vector<FakeDataRun>& runs,
                              const FakeNonResidentOverrides& overrides) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);

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

// Writes "name" as a leaf index entry at "dest" and returns its size in
// bytes. Entries are packed with no padding, like every other fixture here.
WORD WriteFilenameEntry(BYTE* dest, const FakeIndexName& name) {
  auto& entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(dest);
  entry.mft_index = name.mft_ref;
  entry.mft_sn = name.sequence;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&entry.stream);
  filename.parent_ref = static_cast<ULONGLONG>(Mft::Idx::Root);
  filename.flags = name.directory ? NtfsBrowser::Data::FilenameFlag::Directory
                                  : NtfsBrowser::Data::FilenameFlag::None;
  const std::vector<WORD> encoded_name = ToUtf16(name.name);
  filename.name_length = gsl::narrow<BYTE>(encoded_name.size());
  filename.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
  for (size_t i = 0; i < encoded_name.size(); i++) {
    filename.name[i] = encoded_name.at(i);
  }

  entry.stream_size = gsl::narrow<WORD>(
      reinterpret_cast<BYTE*>(&filename.name[filename.name_length]) -
      reinterpret_cast<BYTE*>(&filename));
  entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &entry.stream - reinterpret_cast<BYTE*>(&entry) + entry.stream_size));
  return entry.size;
}

// Writes one resident $INDEX_ROOT at record[offset]: root_names as leaf
// entries, then a nameless SUBNODE-only entry. Returns its total_size.
DWORD WriteIndexRootAttr(FakeRecord& record, DWORD offset,
                         std::span<const FakeIndexName> root_names) {
  // $INDEX_ROOT
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(&record.at(offset));
  root_attr.header.type = Attr::Type::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  const std::span<BYTE> entries =
      body.subspan(sizeof(NtfsBrowser::Data::IndexRoot));
  DWORD leaf_bytes = 0;
  for (const FakeIndexName& name : root_names) {
    leaf_bytes += WriteFilenameEntry(&gsl::at(entries, leaf_bytes), name);
  }

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(entries, leaf_bytes));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Data::IndexEntryFlag::SubNode |
                      NtfsBrowser::Data::IndexEntryFlag::Last;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  root.total_entry_size = leaf_bytes + first_entry.size;
  root.alloc_entry_size = leaf_bytes + first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Data::IndexRoot)) + leaf_bytes +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);
  return root_attr.header.total_size;
}

// Directory record (root, #5): resident $STANDARD_INFORMATION("permission"),
// a resident $INDEX_ROOT holding "rootNames" as leaf entries followed by a
// nameless SUBNODE-only entry, and a non-resident $INDEX_ALLOCATION - lets
// fixtures target an attribute FuzzOnce() actually ReadData()s, unlike plain
// $DATA.
FakeRecord MakeIndexAllocationDirRecord(
    NtfsBrowser::Data::StdInfoPermission permission,
    const FakeNonResidentAttr& index_alloc,
    std::span<const FakeIndexName> root_names) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

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
  // 64KiB read block - same reasoning as
  // BuildFakeNtfsImageWithDeepIndexBlockChain().
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  size_t highest_end = image.size();
  for (const FakeDataRun& run : runs) {
    if (run.lcn) {
      const size_t end = (static_cast<size_t>(*run.lcn) + run.clusters) *
                         static_cast<size_t>(cluster_size);
      highest_end = (end > highest_end) ? end : highest_end;
    }
  }
  const size_t aligned_end = ((highest_end + full_cache_read_block_size - 1) /
                              full_cache_read_block_size) *
                             full_cache_read_block_size;
  if (image.size() < aligned_end) {
    image.resize(aligned_end, 0);
  }

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

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

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
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  block.magic = index_block_magic;
  // Points offset_of_us at the fixup slot itself, so PatchUS() trivially
  // succeeds - same technique as BuildFakeNtfsImageWithGapCollationSubNode().
  block.offset_of_us = static_cast<WORD>(fake_file_record_size - us_slot_size);
  block.size_of_us = 3;
  block.vcn = 0;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));

  DWORD leaf_bytes = 0;
  for (const FakeIndexName& name : names) {
    leaf_bytes += WriteFilenameEntry(&gsl::at(body, leaf_bytes), name);
  }

  auto& last = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, leaf_bytes));
  last.flags = NtfsBrowser::Data::IndexEntryFlag::Last;
  last.stream_size = 0;
  last.size = gsl::narrow<WORD>(
      AlignAttrSize(&last.stream - reinterpret_cast<BYTE*>(&last)));

  block.total_entry_size = leaf_bytes + last.size;
  block.alloc_entry_size = block.total_entry_size;

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
      auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
          &record.at(offset));
      attr.header.type = Attr::Type::Bitmap;
      attr.header.non_resident = 0;
      attr.header.total_size = undersized_total_size;
      break;
    }
    case FakeTrailingDefect::UndersizedCompressedField: {
      auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
          &record.at(offset));
      attr.header.type = Attr::Type::Bitmap;
      attr.header.non_resident = 1;
      attr.comp_unit_size = compression_unit_size_shift;
      attr.header.total_size = NtfsBrowser::Data::header_non_resident_base_size;
      break;
    }
    case FakeTrailingDefect::RejectedAttribute: {
      auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
          &record.at(offset));
      attr.header.type = Attr::Type::StandardInformation;
      attr.header.non_resident = 1;
      attr.header.total_size = AlignAttrSize(
          NtfsBrowser::Data::header_non_resident_base_size + run_list_room);
      break;
    }
  }
}

}  // namespace NtfsBrowserTests
