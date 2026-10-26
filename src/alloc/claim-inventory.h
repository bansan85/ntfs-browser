#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Alloc {

// Clusters one claim holds at most. The count shares its 32-bit word with the
// unwritten flag, so it stops at 2^31 - 1: 8 TiB at 4 KiB. A longer run is
// cut there, and what is left of it is Invalid, for a concerned stream.
inline constexpr ULONGLONG max_claim_clusters = 0x7FFFFFFFU;

// The bit of Claim::clusters_and_flags that marks a claim lying past the
// initialized size of its stream: allocated, and never written.
inline constexpr std::uint32_t claim_unwritten_bit = 0x80000000U;

// The value of "no account": the stream is no concerned $DATA stream.
inline constexpr std::uint32_t no_account =
    (std::numeric_limits<std::uint32_t>::max)();

// A range of VCNs of a stream.
struct VcnRange {
  ULONGLONG vcn{0};
  ULONGLONG clusters{0};
};

// One run of real clusters of one stream, cut where the stream's initialized
// size falls, as the scan met it. 24 bytes: a scan keeps one for every run of
// every non-resident attribute of the volume.
struct Claim {
  ULONGLONG lcn{0};
  // First VCN of the run in its stream.
  ULONGLONG vcn{0};
  // The cluster count in the low 31 bits, and claim_unwritten_bit.
  std::uint32_t clusters_and_flags{0};
  // Index of the stream: local to a RecordClaims, global in a ClaimInventory.
  std::uint32_t stream{0};

  [[nodiscard]] ULONGLONG Clusters() const noexcept {
    return clusters_and_flags & ~claim_unwritten_bit;
  }

  [[nodiscard]] bool Unwritten() const noexcept {
    return (clusters_and_flags & claim_unwritten_bit) != 0;
  }
};

static_assert(sizeof(Claim) == 24);

// What a concerned $DATA stream needs besides its claims, to be counted and
// mapped. Streams of one record that share a name are one, whatever the
// number of parts their attribute had.
struct AccountStream {
  // The stream's name. Empty for the unnamed stream.
  std::wstring name;
  ULONGLONG data_size{0};
  ULONGLONG initialized_size{0};
  // Clusters per compression unit, 0 when not compressed.
  ULONGLONG unit_clusters{0};
  // VCNs the stream spans: its declared size or the end of its runs, the
  // larger. What no run, hole or invalid range covers of [0, span) is Invalid.
  ULONGLONG span{0};
  // Clusters the claims of the stream hold, and the sparse holes and invalid
  // ranges apart from them.
  ULONGLONG real_clusters{0};
  std::vector<VcnRange> sparse;
  std::vector<VcnRange> invalid;
  bool encrypted{false};
  // Index of the owner among the concerned ones, set by the inventory.
  std::uint32_t owner_dense{0};
};

// The claims of one record, collected on the thread that parsed it and added to
// the inventory by the thread that merges the scan.
struct RecordClaims {
  ULONGLONG record{0};
  // The $LogFile sequence number of the record's header.
  ULONGLONG lsn{0};
  bool freed{false};
  // The record gets an account: its $DATA streams are in accounts.
  bool concerned{false};
  // Stream numbers are local: indices into stream_account.
  std::vector<Claim> claims;
  // For each local stream, the index in accounts of its AccountStream, or
  // no_account.
  std::vector<std::uint32_t> stream_account;
  std::vector<AccountStream> accounts;
};

// Collects the claims of the record file_record just parsed: the clusters of
// every non-resident attribute it holds, those of its extension records
// included, but not the bad cluster list of $BadClus. file_record MUST have
// been given the claim mask Attr::Mask::All. With concerned, its $DATA streams
// also get an AccountStream.
template <Cache::Strategy S>
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY RecordClaims
    CollectClaims(const Io::FileRecord<S>& file_record, ULONGLONG record,
                  bool freed, bool concerned);

// Every claim of a scan, in the order the records were added. Plain vectors:
// the sweep reads them directly.
struct ClaimInventory {
  // Who holds the clusters of a stream.
  struct Owner {
    ULONGLONG lsn{0};
    std::uint32_t record{0};
    bool freed{false};
  };

  // A stream's owner, and its account.
  struct StreamRef {
    std::uint32_t owner{0};
    std::uint32_t account{no_account};
  };

  std::vector<Claim> claims;
  std::vector<StreamRef> streams;
  std::vector<Owner> owners;
  std::vector<AccountStream> accounts;
  // The record number of each concerned owner, by owner_dense.
  std::vector<ULONGLONG> concerned_records;

  // Adds one record. A record number that does not fit 32 bits is left out.
  void Add(RecordClaims&& record);
};

}  // namespace Alloc

}  // namespace NTFS_BROWSER_NAMESPACE
