#include "scan-probes.h"

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/directory-read-ahead.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/io/record-standing.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/record/carved-name.h>
#include <ntfs-browser/test/fuzz/looping-disk-reader.h>
#include <ntfs-browser/volume-options.h>

namespace NtfsFuzz {

namespace {

using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Io::RecordStanding;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

// Starting value and multiplier of the 64-bit FNV-1a hash that the digests use.
constexpr std::uint64_t digest_basis = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t digest_prime = 1'099'511'628'211ULL;

#ifdef __clang__
__attribute__((no_sanitize("unsigned-integer-overflow")))
#endif
static std::uint64_t FnvStep(std::uint64_t digest, std::uint64_t value) {
  return (digest ^ value) * digest_prime;
}

// A running FNV-1a digest.
class Fnv {
 public:
  void Mix(std::uint64_t value) { digest_ = FnvStep(digest_, value); }

  void MixTime(const FILETIME& time) {
    Mix(time.dwLowDateTime);
    Mix(time.dwHighDateTime);
  }

  void MixText(std::wstring_view text) {
    for (const wchar_t unit : text) {
      Mix(static_cast<std::uint64_t>(unit));
    }
    Mix(text.size());
  }

  void MixTally(const Alloc::ClusterTally& tally) {
    for (const ULONGLONG count : tally.counts) {
      Mix(count);
    }
  }

  [[nodiscard]] std::uint64_t Value() const { return digest_; }

 private:
  std::uint64_t digest_ = digest_basis;
};

// Mixes everything the tree says of one entry.
void MixEntry(Fnv& digest, const MftTree::Entry& entry) {
  digest.Mix(entry.record);
  digest.Mix(entry.sequence);
  digest.Mix(entry.in_use);
  digest.Mix(entry.directory);
  digest.Mix(entry.size);
  digest.Mix(entry.allocated_size);
  digest.MixTime(entry.create_time);
  digest.MixTime(entry.write_time);
  digest.MixTime(entry.change_time);
  digest.MixTime(entry.access_time);
  digest.Mix(entry.read_only);
  digest.Mix(entry.hidden);
  digest.Mix(entry.system);
  digest.Mix(entry.archive);
  digest.Mix(entry.compressed);
  digest.Mix(entry.encrypted);
  digest.Mix(entry.sparse);
  digest.Mix(
      entry.data_state ? static_cast<std::uint64_t>(*entry.data_state) + 1 : 0);
  for (const MftTree::Name& name : entry.names) {
    digest.MixText(name.name);
    digest.Mix(name.parent_record);
    digest.Mix(name.parent_sequence);
    digest.Mix(name.dos_only);
    digest.Mix(name.parent_valid);
  }
  for (const NTFS_BROWSER_NAMESPACE::Record::CarvedName& carved :
       entry.carved_names) {
    digest.MixText(carved.name);
    digest.Mix(carved.parent_record);
    digest.Mix(carved.parent_known);
    digest.Mix(carved.times_known);
    digest.Mix(carved.partial);
    digest.Mix(carved.name_space);
  }
}

// A digest of everything the tree says, entries and statistics: two scans of
// one input that give the same tree give the same digest.
std::uint64_t Digest(const MftTree& tree) {
  Fnv digest;
  for (const MftTree::Entry& entry : tree.Entries()) {
    MixEntry(digest, entry);
  }
  const MftTree::ScanStats& stats = tree.Stats();
  for (const ULONGLONG count :
       {stats.slots, stats.in_use, stats.deleted, stats.extensions,
        stats.unreadable, stats.skipped, stats.damaged, stats.reused_extensions,
        stats.unreachable, stats.carved_names, stats.data_intact,
        stats.data_reallocated, stats.data_unknown, stats.data_bad}) {
    digest.Mix(count);
  }
  return digest.Value();
}

// A digest of the cluster account of every entry of the tree and of the scan:
// the tally, the map, and the byte ranges the map gives for 4 KiB clusters.
std::uint64_t AccountDigest(const MftTree& tree) {
  constexpr ULONGLONG probed_cluster_size = 4096;
  Fnv digest;
  for (const MftTree::Entry& entry : tree.Entries()) {
    const Alloc::ClusterAccount* const account =
        tree.ClusterAccountOf(entry.record);
    if (account == nullptr) {
      continue;
    }
    digest.Mix(entry.record);
    digest.MixTally(account->tally);
    for (const Alloc::StreamClusterMap& map : account->streams) {
      digest.Mix(map.name.size());
      digest.Mix(map.data_size);
      digest.Mix(map.initialized_size);
      for (const Alloc::ClusterPiece& piece : map.pieces) {
        digest.Mix(piece.first_vcn);
        digest.Mix(piece.clusters);
        digest.Mix(piece.lcn.value_or(0));
        digest.Mix(static_cast<std::uint64_t>(piece.condition));
      }
      for (const Alloc::StreamByteRange& range :
           Alloc::ToByteRanges(map, probed_cluster_size)) {
        digest.Mix(range.offset);
        digest.Mix(range.length);
        digest.Mix(range.damaged);
        digest.Mix(range.reads_as_zeros);
      }
    }
  }
  digest.Mix(tree.Stats().cluster_accounts);
  digest.MixTally(tree.Stats().clusters);
  return digest.Value();
}

// ScanDigestOver() on the input, read in a loop.
template <Cache::Strategy S>
std::optional<std::uint64_t>
    ScanDigest(std::span<const BYTE> data, const VolumeOptions& options,
               std::optional<size_t> failing_read, unsigned threads) {
  return ScanDigestOver<S>(
      std::make_unique<LoopingDiskReader>(data, failing_read), options,
      threads);
}

// Number of threads the second scan of ProbeScanThreads() runs with.
constexpr unsigned probed_scan_threads = 3;

// Directories the read-ahead probe lists at most, and the window and budget it
// reads ahead with: a few dozen directories are enough to slide the window.
constexpr size_t read_ahead_probe_directories = 32;
constexpr size_t read_ahead_probe_window = 4;
constexpr std::uint64_t read_ahead_probe_bytes = std::uint64_t{1} << 20U;

// The file references of the subdirectories of the directory listing holds,
// leaving out the entries that name a record reused by another file.
template <Cache::Strategy S>
std::vector<ULONGLONG> ChildDirectories(FileRecord<S>& listing) {
  std::vector<ULONGLONG> children;
  listing.TraverseSubEntriesWithStanding(
      [](const IndexEntryView& entry, RecordStanding standing, void* context) {
        if (entry.IsDirectory() && standing != RecordStanding::Reused) {
          static_cast<std::vector<ULONGLONG>*>(context)->push_back(
              entry.GetFileReference());
        }
      },
      &children);
  return children;
}

// The next directories the listing enters, at most window of them: the top of
// pending that is not listed yet.
std::vector<ULONGLONG>
    UpcomingDirectories(const std::vector<ULONGLONG>& pending,
                        const std::unordered_set<ULONGLONG>& listed,
                        size_t window) {
  std::vector<ULONGLONG> upcoming;
  for (auto next = pending.rbegin();
       next != pending.rend() && upcoming.size() < window; ++next) {
    if (!listed.contains(*next)) {
      upcoming.push_back(*next);
    }
  }
  return upcoming;
}

// Makes listing the next pending directory that was not listed and that
// parses. False when none is left.
template <Cache::Strategy S>
bool EnterNextDirectory(
    FileRecord<S>& listing,
    NTFS_BROWSER_NAMESPACE::Io::DirectoryReadAhead<S>& ahead,
    std::vector<ULONGLONG>& pending, std::unordered_set<ULONGLONG>& listed) {
  while (!pending.empty()) {
    const ULONGLONG next = pending.back();
    pending.pop_back();
    if (!listed.insert(next).second) {
      ahead.Release(next);
      continue;
    }
    if (listing.ParseFileRecord(next) && listing.ParseAttrs()) {
      return true;
    }
    ahead.Release(next);
  }
  return false;
}

}  // namespace

// Scans the volume over reader with threads threads, as ProbeMftTree() does,
// and gives the digest of the tree. Nothing when the scan throws: only the fact
// that it did is worth comparing.
template <Cache::Strategy S>
std::optional<std::uint64_t>
    ScanDigestOver(std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader,
                   const VolumeOptions& options, unsigned threads) {
  try {
    NtfsVolume<S> volume(std::move(reader), options);
    if (!volume.IsVolumeOK()) {
      return std::nullopt;
    }
    const MftTree tree(
        volume, {.progress = [](ULONGLONG done,
                                ULONGLONG /*total*/) { return done == 0; },
                 .threads = threads});
    return Digest(tree);
  } catch (...) {
    return std::nullopt;
  }
}

template <Cache::Strategy S>
std::optional<std::uint64_t> AccountDigestOver(
    std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader,
    const VolumeOptions& options, unsigned threads) {
  try {
    NtfsVolume<S> volume(std::move(reader), options);
    if (!volume.IsVolumeOK()) {
      return std::nullopt;
    }
    const MftTree tree(
        volume, {.progress = [](ULONGLONG done,
                                ULONGLONG /*total*/) { return done == 0; },
                 .threads = threads,
                 .cluster_account = Alloc::ClusterAccountLevel::CountsAndMap,
                 .verify_data = Alloc::DataCheckMode::Full});
    return AccountDigest(tree);
  } catch (...) {
    return std::nullopt;
  }
}

template <Cache::Strategy S>
void ProbeScanThreads(std::span<const BYTE> data, const VolumeOptions& options,
                      std::optional<size_t> failing_read) {
  const std::optional<std::uint64_t> one =
      ScanDigest<S>(data, options, failing_read, 1);
  const std::optional<std::uint64_t> several =
      ScanDigest<S>(data, options, failing_read, probed_scan_threads);
  if (one != several) {
    std::abort();
  }
}

template <Cache::Strategy S>
void ProbeClusterAccount(std::span<const BYTE> data,
                         const VolumeOptions& options,
                         std::optional<size_t> failing_read) {
  const std::optional<std::uint64_t> one = AccountDigestOver<S>(
      std::make_unique<LoopingDiskReader>(data, failing_read), options, 1);
  const std::optional<std::uint64_t> several = AccountDigestOver<S>(
      std::make_unique<LoopingDiskReader>(data, failing_read), options,
      probed_scan_threads);
  if (one != several) {
    std::abort();
  }
}

template <Cache::Strategy S>
void ProbeStandingWalk(std::span<const BYTE> data, const VolumeOptions& options,
                       std::optional<size_t> failing_read) {
  NtfsVolume<S> volume(std::make_unique<LoopingDiskReader>(data, failing_read),
                       options);
  if (!volume.IsVolumeOK()) {
    return;
  }
  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  if (!root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)) ||
      !root.ParseAttrs()) {
    return;
  }
  std::uint64_t seen = 0;
  root.TraverseSubEntriesWithStanding(
      [](const IndexEntryView& entry, RecordStanding standing, void* context) {
        *static_cast<std::uint64_t*>(context) +=
            entry.GetFilename().size() + static_cast<std::uint64_t>(standing);
      },
      &seen);
}

template <Cache::Strategy S>
void ProbeDirectoryReadAhead(std::span<const BYTE> data,
                             const VolumeOptions& options,
                             std::optional<size_t> failing_read) {
  VolumeOptions tuned = options;
  tuned.tuning.walk.read_ahead_dirs = read_ahead_probe_window;
  tuned.tuning.walk.read_ahead_bytes = read_ahead_probe_bytes;
  NtfsVolume<S> volume(std::make_unique<LoopingDiskReader>(data, failing_read),
                       tuned);
  if (!volume.IsVolumeOK()) {
    return;
  }
  FileRecord<S> listing(volume);
  listing.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  if (!listing.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)) ||
      !listing.ParseAttrs()) {
    return;
  }
  NTFS_BROWSER_NAMESPACE::Io::DirectoryReadAhead<S> ahead(volume);
  ahead.Attach(listing);
  ahead.Begin();

  std::vector<ULONGLONG> pending;
  std::unordered_set<ULONGLONG> listed{static_cast<ULONGLONG>(Mft::Idx::Root)};
  for (size_t directories = 0; directories < read_ahead_probe_directories;
       directories++) {
    const std::vector<ULONGLONG> children = ChildDirectories(listing);
    if (const std::optional<ULONGLONG> current = listing.GetFileReference()) {
      ahead.Release(*current);
    }
    pending.insert(pending.end(), children.rbegin(), children.rend());
    ahead.Plan(UpcomingDirectories(pending, listed, ahead.Window()));
    if (!EnterNextDirectory(listing, ahead, pending, listed)) {
      return;
    }
  }
}

template std::optional<std::uint64_t> ScanDigestOver<Cache::Strategy::NoCache>(
    std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>, const VolumeOptions&,
    unsigned);
template std::optional<std::uint64_t>
    ScanDigestOver<Cache::Strategy::FullCache>(
        std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>,
        const VolumeOptions&, unsigned);
template std::optional<std::uint64_t>
    AccountDigestOver<Cache::Strategy::NoCache>(
        std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>,
        const VolumeOptions&, unsigned);
template std::optional<std::uint64_t>
    AccountDigestOver<Cache::Strategy::FullCache>(
        std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>,
        const VolumeOptions&, unsigned);
template void ProbeScanThreads<Cache::Strategy::NoCache>(std::span<const BYTE>,
                                                         const VolumeOptions&,
                                                         std::optional<size_t>);
template void ProbeScanThreads<Cache::Strategy::FullCache>(
    std::span<const BYTE>, const VolumeOptions&, std::optional<size_t>);
template void ProbeStandingWalk<Cache::Strategy::NoCache>(
    std::span<const BYTE>, const VolumeOptions&, std::optional<size_t>);
template void ProbeStandingWalk<Cache::Strategy::FullCache>(
    std::span<const BYTE>, const VolumeOptions&, std::optional<size_t>);
template void ProbeClusterAccount<Cache::Strategy::NoCache>(
    std::span<const BYTE>, const VolumeOptions&, std::optional<size_t>);
template void ProbeClusterAccount<Cache::Strategy::FullCache>(
    std::span<const BYTE>, const VolumeOptions&, std::optional<size_t>);
template void ProbeDirectoryReadAhead<Cache::Strategy::NoCache>(
    std::span<const BYTE>, const VolumeOptions&, std::optional<size_t>);
template void ProbeDirectoryReadAhead<Cache::Strategy::FullCache>(
    std::span<const BYTE>, const VolumeOptions&, std::optional<size_t>);

}  // namespace NtfsFuzz
