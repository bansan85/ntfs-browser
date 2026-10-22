#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// The most clusters one fixture run can map: its length is written in one byte.
constexpr DWORD max_run_clusters = 255;

// Attribute flag of a sparse stream. The library names no such constant, since
// it reads sparseness from the runs.
constexpr WORD flag_sparse = 0x8000;

// Name of the stream that holds the bad clusters.
constexpr std::wstring_view bad_stream_name = L"$Bad";

// Byte that ends a run list early: its length field claims 15 bytes, which no
// run can have, so a decoder stops there.
constexpr BYTE malformed_run_header = 0x1F;

// Appends a hole of clusters clusters, in runs a length byte can hold.
void AppendHole(std::vector<FakeDataRun>& runs, ULONGLONG clusters) {
  while (clusters != 0) {
    const auto part =
        static_cast<DWORD>(std::min<ULONGLONG>(clusters, max_run_clusters));
    runs.push_back({.lcn = std::nullopt, .clusters = part});
    clusters -= part;
  }
}

// Appends the real runs of one extent, in runs a length byte can hold.
void AppendExtent(std::vector<FakeDataRun>& runs, const Alloc::Extent& extent) {
  ULONGLONG lcn = extent.lcn;
  ULONGLONG clusters = extent.clusters;
  while (clusters != 0) {
    const auto part =
        static_cast<DWORD>(std::min<ULONGLONG>(clusters, max_run_clusters));
    runs.push_back({.lcn = lcn, .clusters = part});
    lcn += part;
    clusters -= part;
  }
}

// A one-run list for the first real run: length and LCN as the 4-byte-LCN form
// encodes them.
std::vector<BYTE> EncodeFirstRun(const std::vector<FakeDataRun>& runs) {
  std::vector<BYTE> encoded;
  for (const FakeDataRun& run : runs) {
    if (!run.lcn) {
      continue;
    }
    const auto lcn = static_cast<DWORD>(*run.lcn);
    encoded = {run_header4_lcn_bytes, static_cast<BYTE>(run.clusters)};
    const auto* const lcn_bytes = reinterpret_cast<const BYTE*>(&lcn);
    encoded.insert(encoded.end(), lcn_bytes, lcn_bytes + sizeof(lcn));
    break;
  }
  return encoded;
}

}  // namespace

std::vector<FakeDataRun> ExtentsAsRuns(std::span<const Alloc::Extent> extents) {
  std::vector<FakeDataRun> runs;
  for (const Alloc::Extent& extent : extents) {
    AppendExtent(runs, extent);
  }
  return runs;
}

void PutBadClusterList(std::vector<BYTE>& image,
                       std::span<const Alloc::Extent> bad_extents,
                       const FakeBadClusterOptions& options) {
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  FakeRecord record = MakeRecordHeader(
      attr_offset_value,
      options.record_not_in_use
          ? NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::None
          : NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  DWORD offset = attr_offset_value;

  // The unnamed stream of $BadClus is empty.
  offset += WriteNonResidentAttr(
      record, offset, Attr::Type::Data,
      {.comp_unit_size = 0, .real_size = 0, .runs = {}, .overrides = {}});

  if (!options.omit_bad_stream) {
    // The position in the stream is the cluster number, so an extent in order
    // is preceded by the hole up to it. One out of order follows with no hole.
    std::vector<FakeDataRun> runs;
    ULONGLONG cursor = 0;
    for (const Alloc::Extent& extent : bad_extents) {
      if (extent.clusters == 0) {
        continue;
      }
      if (extent.lcn > cursor) {
        AppendHole(runs, extent.lcn - cursor);
        cursor = extent.lcn;
      }
      AppendExtent(runs, extent);
      cursor += extent.clusters;
    }

    std::vector<BYTE> raw_runs;
    if (options.cut_run_list) {
      raw_runs = EncodeFirstRun(runs);
      raw_runs.push_back(malformed_run_header);
      raw_runs.push_back(0x00);
    }

    offset += WriteNonResidentAttr(
        record, offset, Attr::Type::Data,
        {.comp_unit_size = 0,
         .real_size = options.forged_size.value_or(image.size()),
         .runs = runs,
         .overrides = {.name = bad_stream_name,
                       .flags = options.attr_flags.value_or(flag_sparse),
                       .raw_runs = std::move(raw_runs)}});
  }

  WriteEndOfAttributesMarker(record, offset);
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::BadCluster),
               record);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
