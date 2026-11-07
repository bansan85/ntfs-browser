#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/bpb-edit.h>
#include <ntfs-browser/tool/fuzzgen/index-edit.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/legacy-geometry.h>
#include <ntfs-browser/tool/fuzzgen/legacy-index-block-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/filename-namespace.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "data/ntfs-bpb.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;

// The root directory record, whose index has the sub-node.
constexpr ULONGLONG root_record = 5;

// The cluster where the builder writes the only index block, and the offset
// in the image where that cluster starts.
constexpr std::size_t index_block_lcn = 20;
constexpr std::size_t index_block_offset =
    index_block_lcn * FakeImage::fake_cluster_size;

// A record or an index block of one sector keeps its update sequence number
// in the last word. The words that the packing may move stop before it.
constexpr std::size_t last_word_offset = 0x3FE;

// The end of the first 512 bytes of a block, where a volume of 512-byte
// sectors keeps the saved word of its first sector.
constexpr std::size_t first_sector_end = 0x1FE;

// Where the first entry of an index block starts: after the block header and
// the entry offset the header declares.
constexpr std::size_t block_entries_offset = 0x28;

// A "INDX" magic that was overwritten.
constexpr DWORD overwritten_magic = 0x58585858;

// An entry size and an entry offset that reach past the 1 KiB block.
constexpr WORD entry_size_past_block = 0x400;
constexpr DWORD entry_offset_past_block = 0x3E8;

// A sector end word that does not match the update sequence number.
constexpr WORD mismatched_sequence_number = 1;

// The first read of the damaged item shows the damage, the next ones do not:
// the first copy of the item in the saved input is damaged, the others intact.
constexpr std::size_t first_read_only = 1;

// The run list of the index allocation: one run of one cluster at the cluster
// of the block, with a 4-byte offset, and the same run with a length of two.
const std::vector<BYTE> one_cluster_run = {0x41, 0x01, 0x14, 0, 0, 0, 0};
const std::vector<BYTE> two_cluster_run = {0x41, 0x02, 0x14, 0, 0, 0, 0};

// Sizes in the hand-made indexes: the fixed part of an entry, the fixed part
// of a $FILE_NAME before its name, and the VCN a sub-node entry ends with.
constexpr WORD entry_header_bytes = 16;
constexpr WORD file_name_header_bytes = 66;
constexpr WORD sub_node_vcn_bytes = 8;

// The size of the root index header, and the bytes of the two headers that
// open an $INDEX_ROOT value.
constexpr WORD root_header_bytes = 32;

// A leaf entry for a file named "AB", with the number of its record, and the
// directory that holds it.
constexpr ULONGLONG ab_record = 0x32;
constexpr ULONGLONG ab_sequence = 1;
constexpr ULONGLONG ab_parent = 5;
constexpr WORD ab_name_length = 2;

// The Win32 name space, and the code units of the name "AB".
constexpr BYTE win32_name_space = 1;
constexpr WORD unit_a = 0x41;
constexpr WORD unit_b = 0x42;

// The block size of a hand-made index: 1 KiB, one cluster. The wide index of
// the last two inputs has blocks of 7 KiB, seven clusters.
constexpr DWORD block_bytes = 0x400;
constexpr BYTE block_clusters = 1;
constexpr DWORD wide_block_bytes = 0x1C00;
constexpr BYTE wide_block_clusters = 7;

// Update sequence array of a block of one sector: the number, and one word.
constexpr WORD block_array_offset = 0x3FC;
constexpr WORD one_sector_array_size = 2;

// The VCN of a sub-node entry that does not fit in a byte offset.
constexpr ULONGLONG vcn_overflowing = std::numeric_limits<ULONGLONG>::max();

// A total of entry bytes that is one less than the end entry needs.
constexpr DWORD total_below_end_entry = 0x0F;

// An update sequence array offset that lies past the end of any block.
constexpr WORD array_offset_past_block = 0xFFFF;

// A real size of seven thousand bytes: not a multiple of the 7 KiB block.
constexpr ULONGLONG real_size_between_blocks = 7000;

// The cluster of the block of the input with an empty sub-node block.
constexpr BYTE empty_block_lcn = 0xE6;

// What a hand-made $INDEX_ROOT holds.
struct RootIndex {
  DWORD block_size = block_bytes;
  BYTE clusters_per_block = block_clusters;
  // An end entry that points to the sub-node at vcn, or none.
  std::optional<ULONGLONG> sub_node_vcn;
  // A leaf entry for "AB", before the end entry.
  bool with_ab_entry = false;
};

// Replaces the attributes of the root with an $INDEX_ROOT of file names.
// Returns the offset of its first entry.
std::size_t PutRootIndex(Image& image, const RootIndex& index) {
  const WORD ab_bytes =
      index.with_ab_entry
          ? static_cast<WORD>(entry_header_bytes + file_name_header_bytes +
                              (2 * ab_name_length))
          : WORD{0};
  const WORD end_bytes = static_cast<WORD>(
      entry_header_bytes + (index.sub_node_vcn ? sub_node_vcn_bytes : 0));
  const DWORD entries_bytes = static_cast<DWORD>(ab_bytes + end_bytes);
  RecordEdit::ClearAttributes(image, root_record);
  const std::size_t attr = RecordEdit::AppendResident(
      image, root_record, RecordEdit::type_index_root, 0,
      std::vector<BYTE>(root_header_bytes + entries_bytes, 0));
  const std::size_t value = RecordEdit::ValueOffset(image, attr);
  RecordEdit::Edit<Data::IndexRoot>(image, value, [&](auto& header) {
    header.attr_type = static_cast<Attr::Type>(RecordEdit::type_file_name);
    header.ib_size = index.block_size;
    header.clusters_per_ib = index.clusters_per_block;
    header.entry_offset = entry_header_bytes;
    header.total_entry_size = entries_bytes;
    header.alloc_entry_size = entries_bytes;
  });
  const std::size_t first = value + root_header_bytes;
  if (index.with_ab_entry) {
    RecordEdit::Edit<Data::IndexEntry>(image, first, [&](auto& entry) {
      entry.mft_index = ab_record;
      entry.mft_sn = ab_sequence;
      entry.size = ab_bytes;
      entry.stream_size =
          static_cast<WORD>(file_name_header_bytes + (2 * ab_name_length));
    });
    RecordEdit::Edit<Data::Filename>(
        image, first + entry_header_bytes, [](auto& name) {
          name.parent_ref = ab_parent;
          name.name_length = static_cast<BYTE>(ab_name_length);
          name.name_space =
              static_cast<Data::FilenameNamespace>(win32_name_space);
        });
    RecordEdit::StoreWord(
        image, first + entry_header_bytes + file_name_header_bytes, unit_a);
    RecordEdit::StoreWord(
        image, first + entry_header_bytes + file_name_header_bytes + 2, unit_b);
  }
  RecordEdit::Edit<Data::IndexEntry>(image, first + ab_bytes, [&](auto& entry) {
    entry.size = end_bytes;
    entry.flags = Data::IndexEntryFlag::Last;
    if (index.sub_node_vcn) {
      entry.flags |= Data::IndexEntryFlag::SubNode;
    }
  });
  if (index.sub_node_vcn) {
    RecordEdit::StoreQword(image, first + ab_bytes + entry_header_bytes,
                           *index.sub_node_vcn);
  }
  return first;
}

// Puts a non-resident $INDEX_ALLOCATION after the index of the root. The
// bytes of runs are the run list, with its end.
void PutAllocation(Image& image, const std::vector<BYTE>& runs,
                   ULONGLONG last_vcn, ULONGLONG size) {
  RecordEdit::AppendNonResident(image, root_record,
                                RecordEdit::type_index_allocation, runs,
                                last_vcn, size, size);
}

// Writes at the cluster lcn an index block of one sector, with a header that
// holds no entry but the end entry, and entries_total as the size of the
// entries it declares. The block is for the VCN 0.
void PutEmptyBlock(Image& image, std::size_t lcn, DWORD entries_total) {
  const std::size_t offset = lcn * FakeImage::fake_cluster_size;
  image.resize(std::max(image.size(), offset + block_bytes));
  RecordEdit::Edit<Data::IndexBlock>(image, offset, [&](auto& block) {
    block.magic = Data::index_block_magic;
    block.offset_of_us = block_array_offset;
    block.size_of_us = one_sector_array_size;
    block.entry_offset = entry_header_bytes;
    block.total_entry_size = entries_total;
    block.alloc_entry_size = entry_header_bytes;
  });
  RecordEdit::Edit<Data::IndexEntry>(image, offset + block_entries_offset,
                                     [](auto& entry) {
                                       entry.size = entry_header_bytes;
                                       entry.flags = Data::IndexEntryFlag::Last;
                                     });
}

// A root whose index has wide blocks and a sub-node, over seven clusters of
// index allocation that start at the cluster of the block.
void PutWideIndex(Image& image, ULONGLONG allocation_real_size) {
  PutRootIndex(image, {.block_size = wide_block_bytes,
                       .clusters_per_block = wide_block_clusters,
                       .sub_node_vcn = 0});
  const std::size_t allocation = RecordEdit::AppendNonResident(
      image, root_record, RecordEdit::type_index_allocation,
      {0x41, wide_block_clusters, 0x14, 0, 0, 0, 0}, wide_block_clusters - 1,
      wide_block_bytes, wide_block_bytes);
  RecordEdit::Edit<Data::HeaderNonResident>(
      image, allocation,
      [&](auto& header) { header.real_size = allocation_real_size; });
  const std::size_t offset = index_block_offset;
  image.resize(std::max(image.size(), offset + wide_block_bytes));
  BpbEdit::Apply(image, [](Data::NtfsBpb& bpb) {
    bpb.clusters_per_index_block = wide_block_clusters;
  });
  RecordEdit::Edit<Data::IndexBlock>(image, offset, [](auto& block) {
    block.magic = Data::index_block_magic;
    block.offset_of_us = array_offset_past_block;
    block.size_of_us = wide_block_clusters + 1;
  });
}

// The crash input of 2026-09-02: a root with two $INDEX_ROOT attributes. The
// first holds a sub-node entry whose name claims 255 characters in an entry of
// 96 bytes. The second declares its first entry past its end. The index block
// declares the same.
constexpr WORD crash_entry_size = 0x60;
constexpr WORD crash_stream_size = 0x50;
constexpr ULONGLONG crash_entry_record = 0x14;
constexpr BYTE crash_name_length = 0xFF;
constexpr DWORD crash_value_size = 0xA0;
constexpr DWORD crash_second_value_size = 0x20;
constexpr DWORD crash_block_lcn = 2;

void PutCrashIndex(Image& image) {
  RecordEdit::ClearAttributes(image, root_record);
  const auto put_root = [&](DWORD value_size, const auto& change) {
    const std::size_t attr = RecordEdit::AppendResident(
        image, root_record, RecordEdit::type_index_root, 0,
        std::vector<BYTE>(value_size, 0));
    RecordEdit::Edit<Data::IndexRoot>(
        image, RecordEdit::ValueOffset(image, attr), [&](auto& header) {
          header.attr_type =
              static_cast<Attr::Type>(RecordEdit::type_file_name);
          header.ib_size = block_bytes;
          header.clusters_per_ib = block_clusters;
          change(header);
        });
    return RecordEdit::ValueOffset(image, attr);
  };
  const std::size_t first = put_root(crash_value_size, [](auto& header) {
    header.entry_offset = entry_header_bytes;
    header.total_entry_size = crash_entry_size;
    header.alloc_entry_size = crash_entry_size;
  });
  RecordEdit::Edit<Data::IndexEntry>(
      image, first + root_header_bytes, [](auto& entry) {
        entry.mft_index = crash_entry_record;
        entry.mft_sn = ab_sequence;
        entry.size = crash_entry_size;
        entry.stream_size = crash_stream_size;
        entry.flags = Data::IndexEntryFlag::SubNode;
      });
  RecordEdit::Edit<Data::Filename>(
      image, first + root_header_bytes + entry_header_bytes, [](auto& name) {
        name.name_length = crash_name_length;
        name.name_space =
            static_cast<Data::FilenameNamespace>(win32_name_space);
      });
  put_root(crash_second_value_size,
           [](auto& header) { header.entry_offset = array_offset_past_block; });
  PutAllocation(image, {0x11, 0x01, crash_block_lcn, 0, 0, 0, 0, 0}, 0,
                block_bytes);
  RecordEdit::Edit<Data::IndexBlock>(
      image, crash_block_lcn * FakeImage::fake_cluster_size, [](auto& block) {
        block.magic = Data::index_block_magic;
        block.offset_of_us = block_array_offset;
        block.size_of_us = one_sector_array_size;
        block.entry_offset = array_offset_past_block;
      });
}

// The first builders did not pad the entries of an index to 8 bytes, nor an
// attribute. Cuts the padding of the entries of the root and of the block,
// and fixes the sizes that counted it.
void Repack(Image& image) {
  IndexEdit::UnpadRoot(image, root_record);
  LegacyGeometry::PackLastAttribute(image, root_record);
  IndexEdit::UnpadBlock(image, index_block_offset);
}

// The repacked volume, then one change by edit. A transient_reads above 0 makes
// the change show to that many reads only (see LegacyFixtures::Damaged).
LegacyFixtures::Damaged Variant(const char* name,
                                std::function<void(Image&)> edit,
                                std::size_t transient_reads = 0) {
  return {.name = name,
          .base = FakeImage::BuildFakeNtfsImageWithGapCollationSubNode,
          .prepare = Repack,
          .damage = std::move(edit),
          .transient_reads = transient_reads};
}

Image Base() { return FakeImage::BuildFakeNtfsImage(); }

std::size_t IndexAllocation(const Image& image) {
  return RecordEdit::Attribute(image, root_record,
                               RecordEdit::type_index_allocation);
}

const std::vector<LegacyFixtures::Damaged>& Inputs() {
  static const std::vector<LegacyFixtures::Damaged> inputs = {
      Variant("gap_collation_subnode", [](Image&) {}),
      Variant(
          "data_run_cluster_exceeds_bounds",
          [](Image& image) {
            RecordEdit::Edit<Data::HeaderNonResident>(
                image, IndexAllocation(image), [](auto& header) {
                  header.last_vcn = std::numeric_limits<ULONGLONG>::max();
                });
          },
          first_read_only),
      Variant(
          "data_run_vcn_exceeds_bound",
          [](Image& image) {
            RecordEdit::SetRunList(image, IndexAllocation(image),
                                   two_cluster_run);
          },
          first_read_only),
      Variant(
          "index_block_entry_exceeds_bounds",
          [](Image& image) {
            RecordEdit::Edit<Data::IndexEntry>(
                image, index_block_offset + block_entries_offset,
                [](auto& entry) { entry.size = entry_size_past_block; });
          },
          first_read_only),
      Variant(
          "index_block_entry_header_exceeds_bounds",
          [](Image& image) {
            RecordEdit::Edit<Data::IndexBlock>(
                image, index_block_offset, [](auto& block) {
                  block.entry_offset = entry_offset_past_block;
                });
          },
          first_read_only),
      Variant(
          "index_block_magic_mismatch",
          [](Image& image) {
            RecordEdit::Edit<Data::IndexBlock>(
                image, index_block_offset,
                [](auto& block) { block.magic = overwritten_magic; });
          },
          first_read_only),
      Variant("index_block_usn_mismatch",
              [](Image& image) {
                RecordEdit::StoreWord(image,
                                      index_block_offset + first_sector_end,
                                      mismatched_sequence_number);
                RecordEdit::StoreWord(image,
                                      index_block_offset + last_word_offset,
                                      mismatched_sequence_number);
              }),
      {.name = "65b60629c20b4730c35650dd68b6f87fe57c07a3",
       .base = Base,
       .damage = PutCrashIndex,
       .early = true},
      {.name = "index_root_entry_ab_match",
       .base = Base,
       .damage =
           [](Image& image) { PutRootIndex(image, {.with_ab_entry = true}); }},
      {.name = "index_root_entry_total_exceeds_declared_size",
       .base = Base,
       .damage =
           [](Image& image) {
             const std::size_t first = PutRootIndex(image, {});
             RecordEdit::Edit<Data::IndexRoot>(
                 image, first - root_header_bytes, [](auto& header) {
                   header.total_entry_size = total_below_end_entry;
                 });
           }},
      {.name = "index_block_subnode_vcn_overflow",
       .base = Base,
       .damage =
           [](Image& image) {
             PutRootIndex(image, {.sub_node_vcn = vcn_overflowing});
             PutAllocation(image, std::vector<BYTE>(8, 0), 0, block_bytes);
           }},
      {.name = "index_block_entry_total_exceeds_declared_size",
       .base = Base,
       .damage =
           [](Image& image) {
             PutRootIndex(image, {.sub_node_vcn = 0});
             PutAllocation(image, {0x41, 0x01, empty_block_lcn, 0, 0, 0, 0}, 0,
                           block_bytes);
             PutEmptyBlock(image, empty_block_lcn, total_below_end_entry);
           }},
      {.name = "index_block_offset_of_us_out_of_bounds",
       .base = Base,
       .damage = [](Image& image) { PutWideIndex(image, wide_block_bytes); },
       .early = true},
      {.name = "index_alloc_block_count_incalculable",
       .base = Base,
       .damage =
           [](Image& image) { PutWideIndex(image, real_size_between_blocks); },
       .early = true},
  };
  return inputs;
}

}  // namespace

void LegacyIndexBlockScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::RegisterDamaged(registry, Inputs());
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
