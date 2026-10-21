#include "standalone-mft-disk-reader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-header.h"
#include "data/header-non-resident.h"
#include "data/ntfs-bpb.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// The sector size the boot sector declares. The update sequence words of a
// record sit every 512 bytes, whatever the sector size, and every kept
// fixture comes from a volume with 512-byte sectors.
constexpr DWORD sector_size = 512;

// Smallest cluster size accepted: one sector.
constexpr DWORD min_cluster_size = 512;

// Largest cluster size accepted: 64 KiB, the biggest a Windows volume formats
// by default.
constexpr DWORD max_cluster_size = 65536;

// Size of the boot area the volume reads first: it asks for 4096 bytes, then
// falls back to 512.
constexpr ULONGLONG full_boot_area_bytes = 4096;

// Largest sectors_per_cluster the BPB writes as a plain count. Above it, the
// byte holds minus the base 2 logarithm of the count.
constexpr DWORD max_literal_sectors_per_cluster = 127;

// Size of an index block the boot sector declares: 4 KiB, what NTFS formats.
constexpr DWORD index_block_bytes = 4096;

// Largest end address any run may reach. The library adds a record offset to
// it and narrows the sum to a LONGLONG, so half of that range is the limit.
constexpr ULONGLONG max_volume_bytes =
    static_cast<ULONGLONG>(std::numeric_limits<LONGLONG>::max()) / 2;

// Bits of the high nibble of a run header: the byte count of the offset field.
constexpr unsigned run_offset_shift = 4;

// Largest byte count of a run length or offset field.
constexpr unsigned max_run_field_bytes = 8;

// Boot sector values a real volume has, none of which the library reads: the
// media descriptor of a fixed disk,
constexpr BYTE media_descriptor_fixed_disk = 0xF8;

// the classic CHS geometry,
constexpr WORD sectors_per_track_value = 63;
constexpr WORD number_of_heads_value = 255;

// the word many volumes hold at offset 0x24,
constexpr DWORD not_used3_value = 0x80008000;

// and an arbitrary serial number.
constexpr ULONGLONG serial_value = 0x4D46545F4D4B5354ULL;

// A little-endian field of count bytes at the start of bytes, or nothing when
// bytes is shorter. The value is unsigned: sign extension is the caller's.
std::optional<ULONGLONG> ReadField(std::span<const BYTE> bytes,
                                   unsigned count) {
  if (count > bytes.size() || count > max_run_field_bytes) {
    return std::nullopt;
  }
  ULONGLONG value = 0;
  for (unsigned index = 0; index < count; index++) {
    value |= static_cast<ULONGLONG>(bytes[index]) << (index * CHAR_BIT);
  }
  return value;
}

// Sign-extends the low count bytes of value.
LONGLONG SignExtend(ULONGLONG value, unsigned count) {
  if (count == 0 || count >= max_run_field_bytes) {
    return static_cast<LONGLONG>(value);
  }
  const unsigned bits = count * CHAR_BIT;
  const ULONGLONG sign_bit = ULONGLONG{1} << (bits - 1);
  return static_cast<LONGLONG>((value ^ sign_bit) - sign_bit);
}

// What BPB byte stands for a count of units that is a power of two. A count
// above limit is written as minus its base 2 logarithm.
BYTE EncodeBpbSize(DWORD count, DWORD limit) {
  if (count <= limit) {
    return gsl::narrow<BYTE>(count);
  }
  return static_cast<BYTE>(-std::countr_zero(count));
}

}  // namespace

void StandaloneMftDiskReader::SetRepairDeclaredExtent(bool repair) noexcept {
  repair_declared_extent_ = repair;
}

// Keeps reason for Failure() and returns false, so that Open() can fail in one
// statement.
bool StandaloneMftDiskReader::Fail(std::string reason) {
  failure_ = std::move(reason);
  return false;
}

bool StandaloneMftDiskReader::Open(std::wstring_view path) {
  failure_.clear();
  runs_.clear();
  boot_.clear();
  repaired_record_.clear();
  file_record_size_ = 0;
  cluster_size_ = 0;
  file_size_ = 0;
  volume_bytes_ = 0;
  boot_area_bytes_ = 0;
  file_.close();

  const std::filesystem::path file_path(path);
  std::error_code error;
  file_size_ = std::filesystem::file_size(file_path, error);
  if (error) {
    return Fail("cannot get the size of the file: " + error.message());
  }
  file_.open(file_path, std::ios::binary);
  if (!file_.is_open()) {
    return Fail("cannot open the file");
  }

  if (file_size_ < Data::FileRecordHeader::min_file_record_header_size) {
    return Fail("the file is smaller than a record header");
  }
  Data::FileRecordHeader header{};
  file_.read(reinterpret_cast<char*>(&header),
             Data::FileRecordHeader::min_file_record_header_size);
  if (!file_) {
    return Fail("cannot read the header of record 0");
  }
  if (header.magic != Data::FileRecordHeader::file_record_magic) {
    return Fail("record 0 does not start with FILE");
  }

  // The record size is the allocated size record 0 declares for itself.
  const DWORD record_size = header.alloc_size;
  if (record_size < sector_size ||
      record_size > Data::FileRecordHeader::max_file_record_size ||
      !std::has_single_bit(record_size)) {
    return Fail("record 0 declares the invalid record size " +
                std::to_string(record_size));
  }
  if (file_size_ < record_size) {
    return Fail("the file is smaller than one record");
  }

  std::vector<BYTE> record(record_size);
  file_.clear();
  file_.seekg(0);
  file_.read(reinterpret_cast<char*>(record.data()),
             gsl::narrow<std::streamsize>(record.size()));
  if (!file_) {
    return Fail("cannot read record 0");
  }

  // Undoes the update sequence: the last two bytes of each 512-byte block hold
  // the sequence number on disk, and the real bytes sit in the array.
  const size_t array_end =
      size_t{header.offset_of_us} + size_t{header.size_of_us} * sizeof(WORD);
  if (header.size_of_us == 0 || array_end > record.size() ||
      Data::FileRecordHeader::UpdateSequenceBlockCount(record.size(),
                                                       header.size_of_us) !=
          record.size() / Data::FileRecordHeader::update_sequence_stride) {
    return Fail("record 0 has an unusable update sequence array");
  }
  WORD sequence_number = 0;
  std::memcpy(&sequence_number, &record.at(header.offset_of_us),
              sizeof(sequence_number));
  for (size_t block = 1;
       block <= record.size() / Data::FileRecordHeader::update_sequence_stride;
       block++) {
    const size_t end_of_block =
        block * Data::FileRecordHeader::update_sequence_stride - sizeof(WORD);
    WORD on_disk = 0;
    std::memcpy(&on_disk, &record.at(end_of_block), sizeof(on_disk));
    if (on_disk != sequence_number) {
      return Fail("record 0 fails its update sequence check");
    }
    std::memcpy(&record.at(end_of_block),
                &record.at(header.offset_of_us + block * sizeof(WORD)),
                sizeof(WORD));
  }

  file_record_size_ = record_size;
  if (!MapDataRuns(record)) {
    return false;
  }
  if (!repaired_record_.empty()) {
    // MapDataRuns() changed record: put its update sequence back.
    for (size_t block = 1;
         block <=
         record.size() / Data::FileRecordHeader::update_sequence_stride;
         block++) {
      const size_t end_of_block =
          block * Data::FileRecordHeader::update_sequence_stride - sizeof(WORD);
      std::memcpy(&record.at(header.offset_of_us + block * sizeof(WORD)),
                  &record.at(end_of_block), sizeof(WORD));
      std::memcpy(&record.at(end_of_block), &sequence_number, sizeof(WORD));
    }
    repaired_record_ = std::move(record);
  }
  BuildBootSector();
  return true;
}

// Reads the unnamed $DATA at VCN 0 of record 0, which maps the whole $MFT, and
// derives the cluster size from it. Fills runs_, volume_bytes_ and
// boot_area_bytes_.
bool StandaloneMftDiskReader::MapDataRuns(std::vector<BYTE>& record) {
  Data::FileRecordHeader header{};
  std::memcpy(&header, record.data(),
              Data::FileRecordHeader::min_file_record_header_size);

  size_t pos = header.offset_of_attr;
  std::optional<Data::HeaderNonResident> data;
  size_t data_pos = 0;
  size_t data_end = 0;
  while (pos + sizeof(Attr::HeaderCommon) <= record.size()) {
    Attr::HeaderCommon common{};
    std::memcpy(&common, &record[pos], sizeof(common));
    if (common.type == Attr::Type::All) {
      break;
    }
    if (common.total_size < sizeof(common) ||
        common.total_size > record.size() - pos) {
      return Fail("an attribute of record 0 overruns the record");
    }
    if (common.type == Attr::Type::Data && common.non_resident != 0 &&
        common.name_length == 0) {
      if (common.total_size < sizeof(Data::HeaderNonResident)) {
        return Fail("the $DATA header of record 0 is cut");
      }
      Data::HeaderNonResident found{};
      std::memcpy(&found, &record[pos], sizeof(found));
      if (found.start_vcn == 0) {
        data = found;
        data_pos = pos;
        data_end = pos + common.total_size;
        break;
      }
    }
    pos += common.total_size;
  }
  if (!data) {
    return Fail("record 0 has no non-resident unnamed $DATA at VCN 0");
  }
  if (data->data_run_offset < sizeof(Data::HeaderNonResident) ||
      data_pos + data->data_run_offset >= data_end) {
    return Fail("the run list of record 0 starts outside its attribute");
  }

  struct RawRun {
    std::optional<ULONGLONG> lcn;
    ULONGLONG clusters;
  };

  std::vector<RawRun> raw_runs;
  ULONGLONG total_clusters = 0;
  LONGLONG lcn = 0;
  const std::span<const BYTE> bytes(record);
  size_t cursor = data_pos + data->data_run_offset;
  while (cursor < data_end && record[cursor] != 0) {
    const BYTE run_header = record[cursor++];
    const unsigned length_bytes = run_header & 0x0FU;
    const unsigned offset_bytes = run_header >> run_offset_shift;
    const std::span<const BYTE> rest = bytes.subspan(cursor, data_end - cursor);
    const std::optional<ULONGLONG> length = ReadField(rest, length_bytes);
    if (length_bytes == 0 || !length || *length == 0) {
      return Fail("a run of record 0 has an invalid length");
    }
    cursor += length_bytes;
    const std::optional<ULONGLONG> delta =
        ReadField(bytes.subspan(cursor, data_end - cursor), offset_bytes);
    if (!delta) {
      return Fail("a run of record 0 has an invalid offset");
    }
    cursor += offset_bytes;
    if (total_clusters > std::numeric_limits<ULONGLONG>::max() - *length) {
      return Fail("the runs of record 0 hold too many clusters");
    }
    total_clusters += *length;
    if (offset_bytes == 0) {
      raw_runs.push_back({.lcn = std::nullopt, .clusters = *length});
      continue;
    }
    lcn += SignExtend(*delta, offset_bytes);
    if (lcn < 0) {
      return Fail("a run of record 0 starts at a negative cluster");
    }
    raw_runs.push_back(
        {.lcn = static_cast<ULONGLONG>(lcn), .clusters = *length});
  }
  if (raw_runs.empty() || !raw_runs.front().lcn) {
    return Fail("the first run of record 0's $DATA is missing or sparse");
  }

  if (repair_declared_extent_ && data->last_vcn + 1 < total_clusters) {
    const ULONGLONG last_vcn = total_clusters - 1;
    std::memcpy(
        &record.at(data_pos + offsetof(Data::HeaderNonResident, last_vcn)),
        &last_vcn, sizeof(last_vcn));
    data->last_vcn = last_vcn;
    // Not empty from here on: Open() swaps the update sequence back in.
    repaired_record_.assign(1, 0);
  }

  // The cluster size is what makes the allocated size a whole cluster count.
  if (data->alloc_size == 0 || data->alloc_size % total_clusters != 0) {
    return Fail("the allocated size of $DATA is not a whole cluster count");
  }
  const ULONGLONG cluster = data->alloc_size / total_clusters;
  if (cluster < min_cluster_size || cluster > max_cluster_size ||
      !std::has_single_bit(cluster)) {
    return Fail("the cluster size " + std::to_string(cluster) +
                " derived from record 0 is invalid");
  }
  cluster_size_ = gsl::narrow<DWORD>(cluster);

  ULONGLONG vcn = 0;
  ULONGLONG lowest_byte = std::numeric_limits<ULONGLONG>::max();
  for (const RawRun& raw : raw_runs) {
    if (raw.lcn) {
      if (*raw.lcn > max_volume_bytes / cluster ||
          raw.clusters > max_volume_bytes / cluster - *raw.lcn) {
        return Fail("a run of record 0 lies beyond any plausible volume");
      }
      runs_.push_back({.lcn = *raw.lcn,
                       .file_offset = vcn * cluster,
                       .clusters = raw.clusters});
      lowest_byte = std::min(lowest_byte, *raw.lcn * cluster);
      volume_bytes_ =
          std::max(volume_bytes_, (*raw.lcn + raw.clusters) * cluster);
    }
    vcn += raw.clusters;
  }
  if (lowest_byte < sector_size) {
    return Fail("a run of record 0 overlaps the boot sector");
  }
  boot_area_bytes_ =
      lowest_byte >= full_boot_area_bytes ? full_boot_area_bytes : sector_size;
  return true;
}

// Writes the boot sector the volume reads at byte 0: the geometry found in
// record 0, $MFT at the first run, and no $MFTMirr.
void StandaloneMftDiskReader::BuildBootSector() {
  Data::NtfsBpb bpb{};
  bpb.jmp[0] = 0xEB;
  bpb.jmp[1] = 0x52;
  bpb.jmp[2] = 0x90;
  std::memcpy(std::data(bpb.signature), Data::ntfs_signature.data(),
              sizeof(bpb.signature));
  bpb.bytes_per_sector = gsl::narrow<WORD>(sector_size);
  bpb.sectors_per_cluster = EncodeBpbSize(cluster_size_ / sector_size,
                                          max_literal_sectors_per_cluster);
  bpb.media_descriptor = media_descriptor_fixed_disk;
  bpb.sectors_per_track = sectors_per_track_value;
  bpb.number_of_heads = number_of_heads_value;
  bpb.not_used3 = not_used3_value;
  bpb.total_sectors = volume_bytes_ / sector_size;
  // No mirror: a null LCN tells the volume there is no copy to fall back on.
  bpb.lcn_mft = runs_.front().lcn;
  bpb.lcn_mft_mirr = 0;
  // A record at least a cluster long counts clusters, a shorter one counts
  // bytes.
  bpb.clusters_per_file_record = file_record_size_ >= cluster_size_
                                     ? file_record_size_ / cluster_size_
                                     : EncodeBpbSize(file_record_size_, 0);
  bpb.clusters_per_index_block = index_block_bytes >= cluster_size_
                                     ? index_block_bytes / cluster_size_
                                     : EncodeBpbSize(index_block_bytes, 0);
  std::memcpy(std::data(bpb.volume_sn), &serial_value, sizeof(serial_value));
  bpb.x_aa = Data::bpb_marker_first;
  bpb.x_55 = Data::bpb_marker_second;

  boot_.assign(gsl::narrow<size_t>(boot_area_bytes_), 0);
  std::memcpy(boot_.data(), &bpb, sizeof(bpb));
}

// Fills dest from the file at file_offset, zero padded to a whole cluster. The
// repaired copy of record 0, if any, replaces its bytes.
bool StandaloneMftDiskReader::ReadFromFile(ULONGLONG file_offset,
                                           std::span<BYTE> dest) const {
  // A volume holds whole clusters: a file cut inside its last cluster reads
  // as zeros up to the end of that cluster.
  const ULONGLONG padded_size =
      (file_size_ + cluster_size_ - 1) / cluster_size_ * cluster_size_;
  if (file_offset > padded_size || dest.size() > padded_size - file_offset) {
    return false;
  }
  const size_t in_file = file_offset >= file_size_
                             ? 0
                             : gsl::narrow<size_t>(std::min<ULONGLONG>(
                                   dest.size(), file_size_ - file_offset));
  if (in_file != 0) {
    file_.clear();
    file_.seekg(gsl::narrow<std::streamoff>(file_offset));
    file_.read(reinterpret_cast<char*>(dest.data()),
               gsl::narrow<std::streamsize>(in_file));
    if (gsl::narrow<size_t>(file_.gcount()) != in_file) {
      return false;
    }
  }
  std::fill(dest.begin() + gsl::narrow<std::ptrdiff_t>(in_file), dest.end(),
            BYTE{0});
  // Record 0 sits at the start of the file: overlay the repaired copy.
  if (file_offset < repaired_record_.size()) {
    const size_t overlap =
        std::min<size_t>(dest.size(), repaired_record_.size() -
                                          gsl::narrow<size_t>(file_offset));
    std::memcpy(dest.data(), &repaired_record_.at(file_offset), overlap);
  }
  return true;
}

bool StandaloneMftDiskReader::ReadInto(LARGE_INTEGER& addr,
                                       std::span<BYTE> dest) const {
  if (addr.QuadPart < 0 || boot_.empty()) {
    return false;
  }
  ULONGLONG pos = static_cast<ULONGLONG>(addr.QuadPart);
  while (!dest.empty()) {
    size_t piece = 0;
    if (pos < boot_area_bytes_) {
      piece = std::min<size_t>(dest.size(), boot_area_bytes_ - pos);
      std::memcpy(dest.data(), &boot_.at(pos), piece);
    } else {
      const auto run = std::ranges::find_if(runs_, [&](const Run& candidate) {
        return pos >= candidate.lcn * cluster_size_ &&
               pos < (candidate.lcn + candidate.clusters) * cluster_size_;
      });
      if (run == runs_.end()) {
        return false;
      }
      const ULONGLONG in_run = pos - run->lcn * cluster_size_;
      const ULONGLONG left_in_run = run->clusters * cluster_size_ - in_run;
      piece =
          gsl::narrow<size_t>(std::min<ULONGLONG>(dest.size(), left_in_run));
      if (!ReadFromFile(run->file_offset + in_run, dest.first(piece))) {
        return false;
      }
    }
    dest = dest.subspan(piece);
    pos += piece;
  }
  return true;
}

std::optional<ULONGLONG> StandaloneMftDiskReader::Size() const {
  if (boot_.empty()) {
    return std::nullopt;
  }
  return volume_bytes_;
}

const std::string& StandaloneMftDiskReader::Failure() const noexcept {
  return failure_;
}

DWORD StandaloneMftDiskReader::FileRecordSize() const noexcept {
  return file_record_size_;
}

DWORD StandaloneMftDiskReader::ClusterSize() const noexcept {
  return cluster_size_;
}

ULONGLONG StandaloneMftDiskReader::FileSize() const noexcept {
  return file_size_;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
