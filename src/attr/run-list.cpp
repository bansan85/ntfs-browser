#include "attr/run-list.h"

#include <ntfs-browser/win-types.h>

#include <cassert>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

#include <ntfs-browser/namespace.h>

#include "data/run-entry.h"
#include "log/ntfs-common.h"
#include "ntfs-browser/alloc/types.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

RunList::RunList(ULONGLONG clusters) noexcept : clusters_(clusters) {}

// Decodes the run list in run_bytes. Errors follow the recover flag, as in
// PickData() and AppendDataRun().
void RunList::Parse(std::span<const BYTE> run_bytes, ULONGLONG vcn_bound,
                    bool recover) {
  std::span<const BYTE> data_run = run_bytes;
  ULONGLONG length = 0;
  std::optional<LONGLONG> lcn_offset;
  DecodeCursor cursor{.vcn_bound = vcn_bound};

  while (!data_run.empty() && data_run.front() != 0) {
    if (!PickData(data_run, length, lcn_offset, recover)) {
      // PickData() already logged which check failed.
      if (!recover) {
        throw std::runtime_error("Data run is malformed.\n");
      }
      break;
    }

    if (!AppendDataRun(length, lcn_offset, cursor, recover)) {
      break;
    }
  }
}

// Rebases other's runs onto clusters_ (this list's VCN count so far) and
// appends them, so the two read as one contiguous stream.
void RunList::Append(const RunList& other) {
  for (Data::RunEntry data_run : other.runs_) {
    data_run.start_vcn += clusters_;
    data_run.last_vcn += clusters_;
    runs_.push_back(data_run);
  }
  clusters_ += other.clusters_;
}

// Number of virtual clusters this list covers, appended lists included.
ULONGLONG RunList::TotalClusters() const noexcept { return clusters_; }

// Clusters from the first VCN through the last run that maps real clusters.
// A sparse tail maps nothing, and last_vcn is only a declared bound.
ULONGLONG RunList::MappedClusters() const noexcept {
  ULONGLONG mapped = 0;
  for (const Data::RunEntry& data_run : runs_) {
    if (data_run.lcn.has_value()) {
      mapped = data_run.last_vcn + 1;
    }
  }
  return mapped;
}

// Lists the real runs as extents and measures how far the list reaches. A run
// is damaged when it is empty or when lcn + clusters wraps past 2^64.
RunList::ExtentList RunList::Extents() const {
  ExtentList list;
  for (const Data::RunEntry& data_run : runs_) {
    if (data_run.clusters == 0) {
      list.well_formed = false;
      continue;
    }
    list.covered_clusters = data_run.last_vcn + 1;
    if (!data_run.lcn) {
      continue;
    }
    if (data_run.clusters >
        std::numeric_limits<ULONGLONG>::max() - *data_run.lcn) {
      list.well_formed = false;
    }
    list.extents.push_back(
        {.lcn = *data_run.lcn, .clusters = data_run.clusters});
  }
  return list;
}

// Lists the real runs with their VCN. A run is damaged when it is empty or
// when lcn + clusters wraps past 2^64: it is then left out.
RunList::VcnRunList RunList::RealRanges() const {
  VcnRunList list;
  for (const Data::RunEntry& data_run : runs_) {
    if (data_run.clusters == 0) {
      list.well_formed = false;
      continue;
    }
    list.covered_clusters = data_run.last_vcn + 1;
    if (!data_run.lcn) {
      continue;
    }
    if (data_run.clusters >
        std::numeric_limits<ULONGLONG>::max() - *data_run.lcn) {
      list.well_formed = false;
      continue;
    }
    if (!list.runs.empty() &&
        list.runs.back().start_vcn + list.runs.back().clusters ==
            data_run.start_vcn) {
      list.runs.back().clusters += data_run.clusters;
      continue;
    }
    list.runs.push_back(
        {.start_vcn = data_run.start_vcn, .clusters = data_run.clusters});
  }
  return list;
}

// Returns the first run whose VCN range holds vcn.
const Data::RunEntry* RunList::Find(ULONGLONG vcn) const noexcept {
  for (const Data::RunEntry& data_run : runs_) {
    if (vcn >= data_run.start_vcn && vcn <= data_run.last_vcn) {
      return &data_run;
    }
  }
  return nullptr;
}

// Counts the real (non-sparse) clusters at the start of a compression unit.
// Returns an empty optional if the unit is not fully mapped, or if a real
// run follows a hole within the unit - layouts a compression unit cannot
// legally have.
std::optional<ULONGLONG>
    RunList::LeadingRealClusters(ULONGLONG unit_first_vcn,
                                 ULONGLONG unit_clusters) const noexcept {
  const ULONGLONG unit_end = unit_first_vcn + unit_clusters;
  ULONGLONG vcn = unit_first_vcn;
  ULONGLONG real_clusters = 0;
  bool saw_hole = false;

  for (const Data::RunEntry& data_run : runs_) {
    if (vcn >= unit_end) {
      break;
    }
    if (data_run.last_vcn < vcn) {
      // Entirely before the unit (or before what has been counted so far).
      continue;
    }
    if (data_run.start_vcn > vcn) {
      Log::Warn("Compression unit at VCN {} is not fully mapped",
                unit_first_vcn);
      return {};
    }

    const ULONGLONG in_run = data_run.last_vcn - vcn + 1;
    const ULONGLONG left = unit_end - vcn;
    const ULONGLONG take = (in_run < left) ? in_run : left;

    if (data_run.lcn) {
      if (saw_hole) {
        Log::Warn("Compression unit at VCN {} has real clusters after a hole",
                  unit_first_vcn);
        return {};
      }
      real_clusters += take;
    } else {
      saw_hole = true;
    }

    vcn += take;
  }

  if (vcn != unit_end) {
    // The run list ran out before the unit did.
    Log::Warn("Compression unit at VCN {} is not fully mapped", unit_first_vcn);
    return {};
  }

  return real_clusters;
}

// Parse a single DataRun unit, and advance dataRun past it. dataRun is bounded
// to the attribute (already validated against the record buffer by
// FileRecord::ParseAttrs); data_run_offset and the run stream itself are
// attacker-controlled and otherwise unbounded.
bool RunList::PickData(std::span<const BYTE>& data_run, ULONGLONG& length,
                       std::optional<LONGLONG>& lcn_offset,
                       bool recover) noexcept {
  // Parse() calls this only while the run list still has bytes.
  assert(!data_run.empty());

  union Length {
    struct {
      BYTE length_bytes : 4;
      BYTE offset_bytes : 4;
    };

    BYTE size;
  };

  const Length size{.size = data_run.front()};
  data_run = data_run.subspan(1);

  if (size.length_bytes > sizeof(ULONGLONG) ||
      size.offset_bytes > sizeof(LONGLONG)) {
    Log::Recoverable(recover, "DataRun decode error 1: 0x{:02X}", size.size);
    return false;
  }

  if (data_run.size() < static_cast<size_t>(size.length_bytes) +
                            static_cast<size_t>(size.offset_bytes)) {
    Log::Recoverable(recover,
                     "DataRun decode error: run exceeds attribute bounds");
    return false;
  }

  length = 0;
  memcpy(&length, data_run.data(), size.length_bytes);

  data_run = data_run.subspan(size.length_bytes);
  // A hole has no offset field at all. An offset field that holds zero is a
  // real run: at LCN 0 for the first one, else at the LCN of the run before.
  if (size.offset_bytes != 0) {
    // The size check above leaves at least offsetBytes bytes in dataRun.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    LONGLONG delta =
        static_cast<CHAR>(data_run[size.offset_bytes - 1U]) < 0 ? -1 : 0;
    memcpy(&delta, data_run.data(), size.offset_bytes);
    lcn_offset = delta;

    data_run = data_run.subspan(size.offset_bytes);
  } else {
    lcn_offset.reset();
  }

  return true;
}

// Applies one decoded run (its length and LCN delta) to the cursor's lcn and
// vcn, and appends it to the run list. Returns false on an error that recovery
// tolerates; when strict, the same error throws.
bool RunList::AppendDataRun(ULONGLONG length,
                            std::optional<LONGLONG> lcn_offset,
                            DecodeCursor& cursor, bool recover) {
  // lcn is never negative here, so only a positive offset can overflow the
  // sum. It MUST be caught before the addition: signed overflow is UB.
  const LONGLONG delta = lcn_offset.value_or(0);
  if (delta > 0 && cursor.lcn > std::numeric_limits<LONGLONG>::max() - delta) {
    Log::Recoverable(recover, "DataRun decode error: LCN overflows");
    if (!recover) {
      throw std::runtime_error("Data run LCN overflows.\n");
    }
    return false;
  }

  cursor.lcn += delta;
  if (cursor.lcn < 0) {
    Log::Recoverable(recover, "DataRun decode error 2");
    if (!recover) {
      throw std::runtime_error("Data run LCN underflows.\n");
    }
    return false;
  }

  Log::Debug("Data length = {} clusters, LCN = {}{}", length, cursor.lcn,
             lcn_offset ? "" : ", Sparse Data");

  // Store LCN, Data size (clusters) into list
  Data::RunEntry data_run;
  data_run.lcn = lcn_offset ? std::optional<ULONGLONG>{cursor.lcn}
                            : std::optional<ULONGLONG>{};
  data_run.clusters = length;
  data_run.start_vcn = cursor.vcn;
  cursor.vcn += length;
  data_run.last_vcn = cursor.vcn - 1;

  if (data_run.last_vcn > cursor.vcn_bound) {
    Log::Recoverable(recover, "DataRun decode error: VCN exceeds bound");
    if (!recover) {
      throw std::runtime_error(
          "Data run VCN exceeds the attribute's declared bound.\n");
    }
    return false;
  }

  runs_.push_back(data_run);
  return true;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
