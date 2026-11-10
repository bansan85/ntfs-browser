#include <algorithm>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/mft-run-decoder.h>

#include "attr/header-common.h"
#include "attr/type.h"
#include "data/file-record-header.h"
#include "data/header-non-resident.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::DiskReaders {

namespace {

// Smallest cluster size accepted: one sector.
constexpr DWORD min_cluster_size = 512;

// Largest cluster size accepted: 64 KiB, the biggest a Windows volume formats
// by default.
constexpr DWORD max_cluster_size = 65536;

// Size of the boot area the volume reads first: it asks for 4096 bytes, then
// falls back to 512.
constexpr ULONGLONG full_boot_area_bytes = 4096;

// Largest end address any run may reach. The library adds a record offset to
// it and narrows the sum to a LONGLONG, so half of that range is the limit.
constexpr ULONGLONG max_volume_bytes =
    static_cast<ULONGLONG>(std::numeric_limits<LONGLONG>::max()) / 2;

// Bits of the high nibble of a run header: the byte count of the offset field.
constexpr unsigned run_offset_shift = 4;

// Largest byte count of a run length or offset field.
constexpr unsigned max_run_field_bytes = 8;

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

}  // namespace

// Keeps reason for Failure() and returns false, so that a stage can fail in
// one statement.
bool MftRunDecoder::Fail(std::string reason) {
  failure_ = std::move(reason);
  return false;
}

std::optional<MftRunDecoder::Layout>
    MftRunDecoder::Decode(std::vector<BYTE>& record) {
  failure_.clear();
  std::optional<DataAttr> data = FindUnnamedData(record);
  if (!data) {
    return std::nullopt;
  }
  const std::optional<RunList> list = ParseRunList(record, *data);
  if (!list) {
    return std::nullopt;
  }
  Layout layout;
  layout.repaired = RepairDeclaredExtent(record, *data, list->total_clusters);
  const std::optional<ULONGLONG> cluster =
      DeriveClusterSize(data->header, list->total_clusters);
  if (!cluster) {
    return std::nullopt;
  }
  layout.cluster_size = gsl::narrow<DWORD>(*cluster);
  if (!BuildRuns(list->runs, *cluster, layout)) {
    return std::nullopt;
  }
  return layout;
}

const std::string& MftRunDecoder::Failure() const noexcept { return failure_; }

std::optional<MftRunDecoder::DataAttr>
    MftRunDecoder::FindUnnamedData(const std::vector<BYTE>& record) {
  const std::optional<DataAttr> data = ScanForData(record);
  if (!data) {
    return std::nullopt;
  }
  if (data->header.data_run_offset <
          sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident) ||
      data->pos + data->header.data_run_offset >= data->end) {
    Fail("the run list of record 0 starts outside its attribute");
    return std::nullopt;
  }
  return data;
}

std::optional<MftRunDecoder::DataAttr>
    MftRunDecoder::ScanForData(const std::vector<BYTE>& record) {
  NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader record_header{};
  std::memcpy(&record_header, record.data(),
              NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                  min_file_record_header_size);

  size_t pos = record_header.offset_of_attr;
  while (pos + sizeof(NTFS_BROWSER_NAMESPACE::Attr::HeaderCommon) <=
         record.size()) {
    NTFS_BROWSER_NAMESPACE::Attr::HeaderCommon common{};
    std::memcpy(&common, &record[pos], sizeof(common));
    if (common.type == NTFS_BROWSER_NAMESPACE::Attr::Type::All) {
      break;
    }
    if (common.total_size < sizeof(common) ||
        common.total_size > record.size() - pos) {
      Fail("an attribute of record 0 overruns the record");
      return std::nullopt;
    }
    if (common.type == NTFS_BROWSER_NAMESPACE::Attr::Type::Data &&
        common.non_resident != 0 && common.name_length == 0) {
      if (common.total_size <
          sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident)) {
        Fail("the $DATA header of record 0 is cut");
        return std::nullopt;
      }
      NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident found{};
      std::memcpy(&found, &record[pos], sizeof(found));
      if (found.start_vcn == 0) {
        return DataAttr{
            .header = found, .pos = pos, .end = pos + common.total_size};
      }
    }
    pos += common.total_size;
  }
  Fail("record 0 has no non-resident unnamed $DATA at VCN 0");
  return std::nullopt;
}

std::optional<MftRunDecoder::RunEntry>
    MftRunDecoder::ReadRunEntry(std::span<const BYTE> rest) {
  const BYTE run_header = rest[0];
  const unsigned length_bytes = run_header & 0x0FU;
  const unsigned offset_bytes = run_header >> run_offset_shift;
  const std::optional<ULONGLONG> length =
      ReadField(rest.subspan(1), length_bytes);
  if (length_bytes == 0 || !length || *length == 0) {
    Fail("a run of record 0 has an invalid length");
    return std::nullopt;
  }
  const std::optional<ULONGLONG> delta =
      ReadField(rest.subspan(1 + length_bytes), offset_bytes);
  if (!delta) {
    Fail("a run of record 0 has an invalid offset");
    return std::nullopt;
  }
  return RunEntry{.length = *length,
                  .delta = *delta,
                  .offset_bytes = offset_bytes,
                  .size = 1 + length_bytes + offset_bytes};
}

std::optional<MftRunDecoder::RunList>
    MftRunDecoder::ParseRunList(const std::vector<BYTE>& record,
                                const DataAttr& data) {
  RunList list;
  LONGLONG lcn = 0;
  const std::span<const BYTE> bytes(record);
  size_t cursor = data.pos + data.header.data_run_offset;
  while (cursor < data.end && record[cursor] != 0) {
    const std::optional<RunEntry> entry =
        ReadRunEntry(bytes.subspan(cursor, data.end - cursor));
    if (!entry) {
      return std::nullopt;
    }
    cursor += entry->size;
    if (list.total_clusters >
        std::numeric_limits<ULONGLONG>::max() - entry->length) {
      Fail("the runs of record 0 hold too many clusters");
      return std::nullopt;
    }
    list.total_clusters += entry->length;
    if (entry->offset_bytes == 0) {
      list.runs.push_back({.lcn = std::nullopt, .clusters = entry->length});
      continue;
    }
    lcn += SignExtend(entry->delta, entry->offset_bytes);
    if (lcn < 0) {
      Fail("a run of record 0 starts at a negative cluster");
      return std::nullopt;
    }
    list.runs.push_back(
        {.lcn = static_cast<ULONGLONG>(lcn), .clusters = entry->length});
  }
  if (list.runs.empty() || !list.runs.front().lcn) {
    Fail("the first run of record 0's $DATA is missing or sparse");
    return std::nullopt;
  }
  return list;
}

bool MftRunDecoder::RepairDeclaredExtent(std::vector<BYTE>& record,
                                         DataAttr& data,
                                         ULONGLONG total_clusters) const {
  if (!repair_declared_extent_ || data.header.last_vcn + 1 >= total_clusters) {
    return false;
  }
  const ULONGLONG last_vcn = total_clusters - 1;
  std::memcpy(
      &record.at(
          data.pos +
          offsetof(NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident, last_vcn)),
      &last_vcn, sizeof(last_vcn));
  data.header.last_vcn = last_vcn;
  return true;
}

std::optional<ULONGLONG> MftRunDecoder::DeriveClusterSize(
    const NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident& data,
    ULONGLONG total_clusters) {
  if (data.alloc_size == 0 || data.alloc_size % total_clusters != 0) {
    Fail("the allocated size of $DATA is not a whole cluster count");
    return std::nullopt;
  }
  const ULONGLONG cluster = data.alloc_size / total_clusters;
  if (cluster < min_cluster_size || cluster > max_cluster_size ||
      !std::has_single_bit(cluster)) {
    Fail("the cluster size " + std::to_string(cluster) +
         " derived from record 0 is invalid");
    return std::nullopt;
  }
  return cluster;
}

bool MftRunDecoder::BuildRuns(const std::vector<RawRun>& raw_runs,
                              ULONGLONG cluster, Layout& layout) {
  ULONGLONG vcn = 0;
  ULONGLONG lowest_byte = std::numeric_limits<ULONGLONG>::max();
  for (const RawRun& raw : raw_runs) {
    if (raw.lcn) {
      if (*raw.lcn > max_volume_bytes / cluster ||
          raw.clusters > max_volume_bytes / cluster - *raw.lcn) {
        return Fail("a run of record 0 lies beyond any plausible volume");
      }
      layout.runs.push_back({.lcn = *raw.lcn,
                             .file_offset = vcn * cluster,
                             .clusters = raw.clusters});
      lowest_byte = std::min(lowest_byte, *raw.lcn * cluster);
      layout.volume_bytes =
          std::max(layout.volume_bytes, (*raw.lcn + raw.clusters) * cluster);
    }
    vcn += raw.clusters;
  }
  if (lowest_byte < mft_sector_size) {
    return Fail("a run of record 0 overlaps the boot sector");
  }
  layout.boot_area_bytes = lowest_byte >= full_boot_area_bytes
                               ? full_boot_area_bytes
                               : mft_sector_size;
  return true;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::DiskReaders
