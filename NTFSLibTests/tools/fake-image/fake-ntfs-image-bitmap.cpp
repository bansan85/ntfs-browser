#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/header-resident.h"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

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

// Sets bit lcn of bitmap.
void SetBit(std::vector<BYTE>& bitmap, ULONGLONG lcn) {
  bitmap.at(lcn / 8) |= static_cast<BYTE>(1U << (lcn % 8));
}

// The record 6 of a bitmap kept inside the record.
FakeRecord MakeResidentBitmapRecord(std::span<const BYTE> bytes) {
  assert(bytes.size() <= max_resident_bitmap_bytes);
  FakeRecord record = MakeInUseFileRecord();
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
  FakeRecord record = MakeInUseFileRecord();
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

// Where the clusters of a non-resident bitmap sit: one run, or two with one
// cluster between them.
struct BitmapParts {
  size_t first_clusters{0};
  size_t second_clusters{0};
  ULONGLONG first_lcn{0};
  ULONGLONG second_lcn{0};
};

// Splits the data clusters of layout into the parts of a bitmap that is
// fragmented, or kept in one run.
BitmapParts SplitBitmapParts(const BitmapLayout& layout, bool fragmented) {
  BitmapParts parts;
  parts.first_clusters =
      fragmented ? (layout.data_clusters + 1) / 2 : layout.data_clusters;
  parts.second_clusters = layout.data_clusters - parts.first_clusters;
  parts.first_lcn = layout.first_lcn;
  parts.second_lcn = layout.first_lcn + parts.first_clusters + 1;
  return parts;
}

// Sets the bits of the clusters the bitmap itself occupies, as on a real
// volume, the cluster between two parts included.
void MarkBitmapClusters(std::vector<BYTE>& bits, const BitmapParts& parts,
                        bool fragmented) {
  for (size_t i = 0; i < parts.first_clusters; i++) {
    SetBit(bits, parts.first_lcn + i);
  }
  for (size_t i = 0; i < parts.second_clusters; i++) {
    SetBit(bits, parts.second_lcn + i);
  }
  if (fragmented) {
    SetBit(bits, parts.first_lcn + parts.first_clusters);
  }
}

// Writes bits into the clusters of parts, the first part then the second.
void CopyBitmapBytes(std::vector<BYTE>& image, const std::vector<BYTE>& bits,
                     const BitmapParts& parts) {
  const size_t first_bytes =
      std::min<size_t>(bits.size(), parts.first_clusters * cluster_size);
  std::memcpy(&image.at(parts.first_lcn * cluster_size), bits.data(),
              first_bytes);
  if (parts.second_clusters != 0 && bits.size() > first_bytes) {
    std::memcpy(&image.at(parts.second_lcn * cluster_size),
                bits.data() + first_bytes, bits.size() - first_bytes);
  }
}

// The runs the record of a bitmap kept in parts maps.
std::vector<FakeDataRun> BitmapRuns(const BitmapParts& parts) {
  std::vector<FakeDataRun> runs{
      {.lcn = parts.first_lcn,
       .clusters = static_cast<DWORD>(parts.first_clusters)}};
  if (parts.second_clusters != 0) {
    runs.push_back({.lcn = parts.second_lcn,
                    .clusters = static_cast<DWORD>(parts.second_clusters)});
  }
  return runs;
}

// A run list cut short: the first run, then a header byte that no run has.
std::vector<BYTE> CutRunList(const BitmapParts& parts) {
  std::vector<BYTE> raw_runs =
      EncodeOneRun(static_cast<DWORD>(parts.first_lcn),
                   static_cast<BYTE>(parts.first_clusters));
  raw_runs.push_back(malformed_run_header);
  raw_runs.push_back(0x00);
  return raw_runs;
}

}  // namespace

void PutClusterBitmap(std::vector<BYTE>& image,
                      std::span<const BYTE> bitmap_bytes,
                      const FakeBitmapOptions& options) {
  const size_t wanted_clusters =
      RoundUpToCacheBlock(static_cast<size_t>(options.volume_clusters));
  if (wanted_clusters * cluster_size > image.size()) {
    image.resize(wanted_clusters * cluster_size, 0);
  }

  if (options.shape == FakeBitmapShape::Resident) {
    PutMftRecord(image, static_cast<ULONGLONG>(Mft::Idx::Bitmap),
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

  const BitmapParts parts = SplitBitmapParts(layout, fragmented);
  if (options.pad_to_volume) {
    MarkBitmapClusters(bits, parts, fragmented);
  }
  CopyBitmapBytes(image, bits, parts);

  std::vector<BYTE> raw_runs;
  if (options.shape == FakeBitmapShape::CutRunList) {
    raw_runs = CutRunList(parts);
  }

  PutMftRecord(
      image, static_cast<ULONGLONG>(Mft::Idx::Bitmap),
      MakeNonResidentBitmapRecord(options.forged_size.value_or(bits.size()),
                                  BitmapRuns(parts), std::move(raw_runs)));
}

void SetRecordInUse(std::vector<BYTE>& image, ULONGLONG idx, bool in_use) {
  EditFileRecordHeader(MftRecordBytes(image, idx), [in_use](auto& header) {
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
  FakeRecord mft = MakeInUseFileRecord();
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
  PutMftRecord(built.image, static_cast<ULONGLONG>(Mft::Idx::Mft), mft);

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
