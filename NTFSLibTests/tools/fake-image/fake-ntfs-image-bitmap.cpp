#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/header-resident.h"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image-internal.h"
#include "file-record-header-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// FullCache reads whole blocks of this size, so an image is always a whole
// number of them.
constexpr size_t cache_block_bytes = size_t{64} * 1024;

// Clusters in one cache block.
constexpr size_t cache_block_clusters = cache_block_bytes / cluster_size;

// The most clusters one fixture run can map: its length is written in one byte.
constexpr size_t max_run_clusters = 255;

// The most bitmap bytes a resident $DATA can hold inside a 1 KiB record.
constexpr size_t max_resident_bitmap_bytes = 800;

// Byte that ends a run list early: its length field claims 15 bytes, which no
// run can have, so a decoder stops there.
constexpr BYTE malformed_run_header = 0x1F;

// Rounds clusters up to a whole number of cache blocks.
size_t RoundUpToCacheBlock(size_t clusters) {
  return ((clusters + cache_block_clusters - 1) / cache_block_clusters) *
         cache_block_clusters;
}

// Declares in the boot sector that the volume spans the whole image.
void FitVolumeToImage(std::vector<BYTE>& image) {
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.total_sectors = image.size() / bytes_per_sector;
}

// A one-run list: length and LCN as the 4-byte-LCN form encodes them.
std::vector<BYTE> EncodeOneRun(DWORD lcn, BYTE clusters) {
  std::vector<BYTE> run{run_header4_lcn_bytes, clusters};
  const auto* const lcn_bytes = reinterpret_cast<const BYTE*>(&lcn);
  run.insert(run.end(), lcn_bytes, lcn_bytes + sizeof(lcn));
  return run;
}

// Bytes the image holds for MFT record idx.
std::span<BYTE> RecordBytes(std::vector<BYTE>& image, ULONGLONG idx) {
  const size_t offset = static_cast<size_t>(mft_lcn) * cluster_size +
                        static_cast<size_t>(fake_file_record_size) * idx;
  return std::span<BYTE>(image).subspan(offset, fake_file_record_size);
}

// Sets bit lcn of bitmap.
void SetBit(std::vector<BYTE>& bitmap, ULONGLONG lcn) {
  bitmap.at(lcn / 8) |= static_cast<BYTE>(1U << (lcn % 8));
}

// The record 6 of a bitmap kept inside the record.
FakeRecord MakeResidentBitmapRecord(std::span<const BYTE> bytes) {
  assert(bytes.size() <= max_resident_bitmap_bytes);
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  DWORD offset = attr_offset_value;
  const DWORD data_offset = offset;
  offset +=
      WriteResidentDataAttr(record, offset, static_cast<DWORD>(bytes.size()));
  if (!bytes.empty()) {
    std::memcpy(
        &record.at(data_offset +
                   sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderResident)),
        bytes.data(), bytes.size());
  }
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// The record 6 of a bitmap kept in clusters, as the runs and overrides say.
FakeRecord MakeNonResidentBitmapRecord(ULONGLONG real_size,
                                       const std::vector<FakeDataRun>& runs,
                                       std::vector<BYTE> raw_runs) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  DWORD offset = attr_offset_value;
  offset +=
      WriteNonResidentAttr(record, offset, Attr::Type::Data,
                           {.comp_unit_size = 0,
                            .real_size = real_size,
                            .runs = runs,
                            .overrides = {.raw_runs = std::move(raw_runs)}});
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Where a non-resident bitmap's data goes, and what is left to do around it.
struct BitmapLayout {
  // Clusters of data, and the first one's LCN.
  size_t data_clusters{0};
  ULONGLONG first_lcn{0};
  // Clusters of the volume once the data is appended and the image rounded up.
  size_t total_clusters{0};
  // Bytes the bitmap ends up with.
  size_t bytes{0};
};

// Works out how many clusters the bitmap needs, since the clusters it adds to
// the volume need bits of their own. The count only grows, so this stops.
BitmapLayout PlanBitmap(size_t base_clusters, size_t given_bytes, bool pad,
                        bool fragmented) {
  BitmapLayout layout;
  layout.first_lcn = base_clusters;
  const size_t gap = fragmented ? 1 : 0;
  const size_t least_clusters = fragmented ? 2 : 1;
  layout.data_clusters = least_clusters;
  for (;;) {
    layout.total_clusters =
        RoundUpToCacheBlock(base_clusters + layout.data_clusters + gap);
    layout.bytes = pad ? std::max(given_bytes, (layout.total_clusters + 7) / 8)
                       : given_bytes;
    const size_t needed = std::max<size_t>(
        least_clusters, (layout.bytes + cluster_size - 1) / cluster_size);
    if (needed == layout.data_clusters) {
      return layout;
    }
    layout.data_clusters = needed;
  }
}

}  // namespace

void PutClusterBitmap(std::vector<BYTE>& image,
                      std::span<const BYTE> bitmap_bytes,
                      const FakeBitmapOptions& options) {
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  const size_t wanted_clusters =
      RoundUpToCacheBlock(static_cast<size_t>(options.volume_clusters));
  if (wanted_clusters * cluster_size > image.size()) {
    image.resize(wanted_clusters * cluster_size, 0);
  }

  if (options.shape == FakeBitmapShape::Resident) {
    PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Bitmap),
                 MakeResidentBitmapRecord(bitmap_bytes));
    FitVolumeToImage(image);
    return;
  }

  const bool fragmented = options.shape != FakeBitmapShape::NonResident;
  const size_t base_clusters = image.size() / cluster_size;
  const BitmapLayout layout = PlanBitmap(base_clusters, bitmap_bytes.size(),
                                         options.pad_to_volume, fragmented);
  assert(layout.data_clusters <= 2 * max_run_clusters);

  image.resize(layout.total_clusters * cluster_size, 0);
  FitVolumeToImage(image);

  std::vector<BYTE> bits(bitmap_bytes.begin(), bitmap_bytes.end());
  bits.resize(layout.bytes, 0);

  // The clusters of the bitmap itself are in use, as on a real volume.
  const size_t first_part =
      fragmented ? (layout.data_clusters + 1) / 2 : layout.data_clusters;
  const size_t second_part = layout.data_clusters - first_part;
  const ULONGLONG second_lcn = layout.first_lcn + first_part + 1;
  if (options.pad_to_volume) {
    for (size_t i = 0; i < first_part; i++) {
      SetBit(bits, layout.first_lcn + i);
    }
    for (size_t i = 0; i < second_part; i++) {
      SetBit(bits, second_lcn + i);
    }
    if (fragmented) {
      SetBit(bits, layout.first_lcn + first_part);
    }
  }

  const size_t first_bytes =
      std::min<size_t>(bits.size(), first_part * cluster_size);
  std::memcpy(&image.at(layout.first_lcn * cluster_size), bits.data(),
              first_bytes);
  if (second_part != 0 && bits.size() > first_bytes) {
    std::memcpy(&image.at(second_lcn * cluster_size), bits.data() + first_bytes,
                bits.size() - first_bytes);
  }

  std::vector<FakeDataRun> runs{
      {.lcn = layout.first_lcn, .clusters = static_cast<DWORD>(first_part)}};
  if (second_part != 0) {
    runs.push_back(
        {.lcn = second_lcn, .clusters = static_cast<DWORD>(second_part)});
  }
  std::vector<BYTE> raw_runs;
  if (options.shape == FakeBitmapShape::CutRunList) {
    raw_runs = EncodeOneRun(static_cast<DWORD>(layout.first_lcn),
                            static_cast<BYTE>(first_part));
    raw_runs.push_back(malformed_run_header);
    raw_runs.push_back(0x00);
  }

  PutMftRecord(
      image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Bitmap),
      MakeNonResidentBitmapRecord(options.forged_size.value_or(bits.size()),
                                  runs, std::move(raw_runs)));
}

void SetRecordInUse(std::vector<BYTE>& image, ULONGLONG idx, bool in_use) {
  EditFileRecordHeader(RecordBytes(image, idx), [in_use](auto& header) {
    using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;
    header.flags = in_use ? (header.flags | FileRecordFlag::InUse)
                          : (header.flags & ~FileRecordFlag::InUse);
  });
}

std::vector<BYTE> AllocatedBitmapFor(const std::vector<BYTE>& image,
                                     std::span<const FakeDataRun> runs) {
  const size_t clusters = image.size() / cluster_size;
  std::vector<BYTE> bitmap(((clusters + 63) / 64) * 8, 0);
  for (const FakeDataRun& run : runs) {
    if (!run.lcn) {
      continue;
    }
    for (ULONGLONG i = 0; i < run.clusters; i++) {
      if ((*run.lcn + i) / 8 < bitmap.size()) {
        SetBit(bitmap, *run.lcn + i);
      }
    }
  }
  return bitmap;
}

FakeManyDeleted BuildFakeNtfsImageWithManyDeletedFiles(size_t count,
                                                       DWORD run_clusters) {
  assert(run_clusters >= 1 && run_clusters <= max_run_clusters);
  FakeManyDeleted built;
  built.image = BuildFakeNtfsImage();
  built.run_clusters = run_clusters;

  // $MFT maps every record slot in one run: records 0 to 15, then the files.
  const size_t records = many_deleted_first_idx + count;
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  FakeRecord mft = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  // Length in two bytes, LCN in one: the run header's nibbles say so.
  const std::vector<BYTE> mft_runs{0x12, static_cast<BYTE>(records & 0xFF),
                                   static_cast<BYTE>(records >> 8),
                                   static_cast<BYTE>(mft_lcn), 0x00};
  assert(records <= 0xFFFF);
  DWORD offset = attr_offset_value;
  offset += WriteNonResidentAttr(
      mft, offset, Attr::Type::Data,
      {.comp_unit_size = 0,
       .real_size = ULONGLONG{records} * fake_file_record_size,
       .runs = {{.lcn = mft_lcn, .clusters = static_cast<DWORD>(records)}},
       .overrides = {.raw_runs = mft_runs}});
  WriteEndOfAttributesMarker(mft, offset);
  PutMftRecord(built.image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Mft),
               mft);

  // The files' data lives past the records, twice as many slots as files so
  // they scatter. 7919 is prime, so the stride visits every slot once.
  const size_t records_end = static_cast<size_t>(mft_lcn) + records;
  const size_t slots = 2 * count + 1;
  const size_t total_clusters =
      RoundUpToCacheBlock(records_end + slots * run_clusters);
  built.image.resize(total_clusters * cluster_size, 0);
  FitVolumeToImage(built.image);

  constexpr size_t scatter_stride = 7919;
  for (size_t i = 0; i < count; i++) {
    const ULONGLONG lcn =
        records_end + ((i * scatter_stride) % slots) * run_clusters;
    built.lcns.push_back(lcn);
    PutFreedNonResidentRecord(
        built.image, many_deleted_first_idx + i,
        {.real_size = ULONGLONG{run_clusters} * cluster_size,
         .runs = {{.lcn = lcn, .clusters = run_clusters}}});
  }
  return built;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
