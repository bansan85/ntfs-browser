#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/mft-run-decoder.h>
#include <ntfs-browser/test/disk-readers/standalone-mft-disk-reader.h>

#include "data/file-record-header.h"
#include "data/ntfs-bpb.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::DiskReaders {

namespace {

// The sector size the boot sector declares.
constexpr DWORD sector_size = mft_sector_size;

// Largest sectors_per_cluster the BPB writes as a plain count. Above it, the
// byte holds minus the base 2 logarithm of the count.
constexpr DWORD max_literal_sectors_per_cluster = 127;

// Size of an index block the boot sector declares: 4 KiB, what NTFS formats.
constexpr DWORD index_block_bytes = 4096;

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

// What BPB byte stands for a count of units that is a power of two. A count
// above limit is written as minus its base 2 logarithm.
BYTE EncodeBpbSize(DWORD count, DWORD limit) {
  if (count <= limit) {
    return gsl::narrow<BYTE>(count);
  }
  return static_cast<BYTE>(-std::countr_zero(count));
}

// Undoes the update sequence: the last two bytes of each 512-byte block hold
// the sequence number on disk, and the real bytes sit in the array. Returns why
// the record is unusable, or nothing.
std::optional<std::string_view> UndoUpdateSequence(
    std::vector<BYTE>& record,
    const NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader& header) {
  const size_t array_end =
      size_t{header.offset_of_us} + size_t{header.size_of_us} * sizeof(WORD);
  if (header.size_of_us == 0 || array_end > record.size() ||
      NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::UpdateSequenceBlockCount(
          record.size(), header.size_of_us) !=
          record.size() / NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                              update_sequence_stride) {
    return "record 0 has an unusable update sequence array";
  }
  WORD sequence_number = 0;
  std::memcpy(&sequence_number, &record.at(header.offset_of_us),
              sizeof(sequence_number));
  for (size_t block = 1;
       block <= record.size() / NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                                    update_sequence_stride;
       block++) {
    const size_t end_of_block =
        block * NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                    update_sequence_stride -
        sizeof(WORD);
    WORD on_disk = 0;
    std::memcpy(&on_disk, &record.at(end_of_block), sizeof(on_disk));
    if (on_disk != sequence_number) {
      return "record 0 fails its update sequence check";
    }
    std::memcpy(&record.at(end_of_block),
                &record.at(header.offset_of_us + block * sizeof(WORD)),
                sizeof(WORD));
  }
  return std::nullopt;
}

// Puts the update sequence back on a record UndoUpdateSequence() accepted: the
// real last bytes of each block return to the array, and the sequence number
// to the block.
void RedoUpdateSequence(
    std::vector<BYTE>& record,
    const NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader& header) {
  WORD sequence_number = 0;
  std::memcpy(&sequence_number, &record.at(header.offset_of_us),
              sizeof(sequence_number));
  for (size_t block = 1;
       block <= record.size() / NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                                    update_sequence_stride;
       block++) {
    const size_t end_of_block =
        block * NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                    update_sequence_stride -
        sizeof(WORD);
    std::memcpy(&record.at(header.offset_of_us + block * sizeof(WORD)),
                &record.at(end_of_block), sizeof(WORD));
    std::memcpy(&record.at(end_of_block), &sequence_number, sizeof(WORD));
  }
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

// Forgets everything that an earlier Open() learned.
void StandaloneMftDiskReader::Reset() {
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
}

// Measures and opens the file at path.
bool StandaloneMftDiskReader::OpenFile(std::wstring_view path) {
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
  return true;
}

bool StandaloneMftDiskReader::Open(std::wstring_view path) {
  Reset();
  if (!OpenFile(path)) {
    return false;
  }

  NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader header{};
  std::vector<BYTE> record;
  if (!ReadRecordZero(header, record)) {
    return false;
  }
  if (const std::optional<std::string_view> broken =
          UndoUpdateSequence(record, header)) {
    return Fail(std::string(*broken));
  }

  file_record_size_ = gsl::narrow<DWORD>(record.size());
  MftRunDecoder decoder(repair_declared_extent_);
  std::optional<MftRunDecoder::Layout> layout = decoder.Decode(record);
  if (!layout) {
    return Fail(decoder.Failure());
  }
  runs_ = std::move(layout->runs);
  cluster_size_ = layout->cluster_size;
  volume_bytes_ = layout->volume_bytes;
  boot_area_bytes_ = layout->boot_area_bytes;
  if (layout->repaired) {
    // Decode() changed record: put its update sequence back.
    RedoUpdateSequence(record, header);
    repaired_record_ = std::move(record);
  }
  BuildBootSector();
  return true;
}

// Reads record 0 from the start of the file: first its header, which declares
// the record size, then the whole record, update sequence still on it.
bool StandaloneMftDiskReader::ReadRecordZero(
    NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader& header,
    std::vector<BYTE>& record) {
  if (file_size_ < NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                       min_file_record_header_size) {
    return Fail("the file is smaller than a record header");
  }
  file_.read(reinterpret_cast<char*>(&header),
             NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                 min_file_record_header_size);
  if (!file_) {
    return Fail("cannot read the header of record 0");
  }
  if (header.magic !=
      NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::file_record_magic) {
    return Fail("record 0 does not start with FILE");
  }

  // The record size is the allocated size record 0 declares for itself.
  const DWORD record_size = header.alloc_size;
  if (record_size < sector_size ||
      record_size > NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                        max_file_record_size ||
      !std::has_single_bit(record_size)) {
    return Fail("record 0 declares the invalid record size " +
                std::to_string(record_size));
  }
  if (file_size_ < record_size) {
    return Fail("the file is smaller than one record");
  }

  record.resize(record_size);
  file_.clear();
  file_.seekg(0);
  file_.read(reinterpret_cast<char*>(record.data()),
             gsl::narrow<std::streamsize>(record.size()));
  if (!file_) {
    return Fail("cannot read record 0");
  }
  return true;
}

// Writes the boot sector the volume reads at byte 0: the geometry found in
// record 0, $MFT at the first run, and no $MFTMirr.
void StandaloneMftDiskReader::BuildBootSector() {
  NTFS_BROWSER_NAMESPACE::Data::NtfsBpb bpb{};
  bpb.jmp[0] = 0xEB;
  bpb.jmp[1] = 0x52;
  bpb.jmp[2] = 0x90;
  std::memcpy(std::data(bpb.signature),
              NTFS_BROWSER_NAMESPACE::Data::ntfs_signature.data(),
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
  bpb.x_aa = NTFS_BROWSER_NAMESPACE::Data::bpb_marker_first;
  bpb.x_55 = NTFS_BROWSER_NAMESPACE::Data::bpb_marker_second;

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
      const auto run =
          std::ranges::find_if(runs_, [&](const MftRunDecoder::Run& candidate) {
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

}  // namespace NTFS_BROWSER_NAMESPACE::Test::DiskReaders
