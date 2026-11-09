#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "attr/mask.h"
#include "attr/type.h"
#include "index-entry.h"
#include "io/file-record-index.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Io {

enum class RecordStanding : std::uint8_t;

}  // namespace Io
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

// What the tests of the probes of a directory walk share: a volume over a
// counting reader, the blocks of a directory's index, and a walk that notes
// what its probes ask.
namespace NTFS_BROWSER_NAMESPACE::Test::Io {

// First slot of $MFT that a probe reads through its extents.
inline constexpr ULONGLONG first_mapped_slot = 16;

// A volume over an image, and the reader that counts what it is asked.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
struct OpenVolume {
  Test::DiskReaders::CountingDiskReader* reads{nullptr};
  std::unique_ptr<NtfsVolume<S>> volume;
};

// Opens image as a volume. kind is what the reader says about its device.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
[[nodiscard]] OpenVolume<S>
    Open(const std::vector<BYTE>& image,
         NTFS_BROWSER_NAMESPACE::Tuning::MediaKind kind =
             NTFS_BROWSER_NAMESPACE::Tuning::MediaKind::Unknown) {
  auto memory = std::make_unique<Test::DiskReaders::MemoryDiskReader>(image);
  memory->SetMediaKind(kind);
  auto counting = std::make_unique<Test::DiskReaders::CountingDiskReader>(
      std::move(memory));
  OpenVolume<S> open;
  open.reads = counting.get();
  open.volume =
      std::make_unique<NtfsVolume<S>>(std::move(counting), VolumeOptions{});
  REQUIRE(open.volume->IsVolumeOK());
  return open;
}

// The index blocks of the directory record directory, by sub-node VCN, as the
// disk holds them.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
[[nodiscard]] std::map<ULONGLONG, std::vector<BYTE>>
    BlocksOf(const NtfsVolume<S>& volume, ULONGLONG directory) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  record.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                     NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(record.ParseFileRecord(directory));
  REQUIRE(record.ParseAttrs());

  std::map<ULONGLONG, std::vector<BYTE>> blocks;
  const auto& allocs =
      record.GetAttr(NTFS_BROWSER_NAMESPACE::Attr::Type::IndexAllocation);
  if (allocs.empty()) {
    return blocks;
  }
  const DWORD block_size = volume.GetIndexBlockSize();
  const ULONGLONG step = block_size >= volume.GetClusterSize()
                             ? block_size / volume.GetClusterSize()
                             : 1;
  const ULONGLONG count = allocs.front()->GetDataSize() / block_size;
  for (ULONGLONG index = 0; index < count; index++) {
    std::vector<BYTE> bytes(block_size);
    const std::span<BYTE> out(bytes);
    if (allocs.front()->ReadData(index * block_size, out) == block_size) {
      blocks.emplace(index * step, std::move(bytes));
    }
  }
  return blocks;
}

// What a batched walk of one directory did, and what the held index said of
// it beforehand.
struct DirectoryWalk {
  // DirectoryIndex::HeldProbeGroups() before the walk.
  std::vector<std::vector<ULONGLONG>> groups;
  // The calls the groups took, and what they logged and salvaged.
  std::size_t calls_of_groups{0};
  std::string log_of_groups;
  std::uint64_t recovered_by_groups{0};
  // The records the probes asked the store for, in order: the records a chunk
  // holds from the first mapped slot on.
  std::vector<ULONGLONG> asked;
  // The runs the probes made, as (first slot, count), in order.
  std::vector<std::pair<ULONGLONG, ULONGLONG>> ran;
  // The announcements and reads of the walk.
  std::vector<Test::DiskReaders::CountingDiskReader::Call> calls;
  // The file references the walk delivered.
  std::vector<ULONGLONG> listed;
};

// How a walk of WalkDirectory() is set up.
struct WalkSetup {
  // The blocks the index block source holds. Null: no source.
  const std::map<ULONGLONG, std::vector<BYTE>>* held{nullptr};
  // Records the store finds, by slot. Null: it finds none.
  const std::map<ULONGLONG, std::vector<BYTE>>* held_records{nullptr};
  // Whether the store keeps the runs, so that they are widened to clusters.
  bool keeps{false};
  // A store that sets nothing but the note of the runs it read.
  bool note_only{false};
};

// Parses directory, asks its held index for the probe groups, then walks it
// with TraverseSubEntriesWithStanding() over a probe store that notes what the
// probes ask and read.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
[[nodiscard]] DirectoryWalk WalkDirectory(OpenVolume<S>& open,
                                          ULONGLONG directory,
                                          const WalkSetup& setup) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(*open.volume);
  record.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                     NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  if (setup.held != nullptr) {
    record.SetIndexBlockSource(
        [held = setup.held](ULONGLONG, ULONGLONG vcn) -> std::span<const BYTE> {
          const auto block = held->find(vcn);
          return block == held->end() ? std::span<const BYTE>()
                                      : std::span<const BYTE>(block->second);
        });
  }
  REQUIRE(record.ParseFileRecord(directory));
  REQUIRE(record.ParseAttrs());

  DirectoryWalk walk;
  (void)Test::Support::TakeCapturedLog();
  open.reads->Reset();
  const std::uint64_t recovered =
      NTFS_BROWSER_NAMESPACE::Log::Detail::RecoveredCount();
  walk.groups =
      NTFS_BROWSER_NAMESPACE::Io::DirectoryIndex<S>(record).HeldProbeGroups();
  walk.calls_of_groups = open.reads->Calls().size();
  walk.log_of_groups = Test::Support::TakeCapturedLog();
  walk.recovered_by_groups =
      NTFS_BROWSER_NAMESPACE::Log::Detail::RecoveredCount() - recovered;

  typename NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>::ProbeStore store;
  if (!setup.note_only) {
    store.find = [&walk, &setup](ULONGLONG slot) -> std::span<const BYTE> {
      walk.asked.push_back(slot);
      if (setup.held_records != nullptr) {
        const auto bytes = setup.held_records->find(slot);
        if (bytes != setup.held_records->end()) {
          return bytes->second;
        }
      }
      return {};
    };
    if (setup.keeps) {
      store.keep = [](ULONGLONG, std::span<const BYTE>) {};
    }
  }
  store.ran = [&walk](ULONGLONG first, ULONGLONG count) {
    walk.ran.emplace_back(first, count);
  };
  record.SetProbeStore(store);

  open.reads->Reset();
  record.TraverseSubEntriesWithStanding(
      [](const IndexEntryView& entry,
         NTFS_BROWSER_NAMESPACE::Io::RecordStanding, void* context) {
        static_cast<DirectoryWalk*>(context)->listed.push_back(
            entry.GetFileReference());
      },
      &walk);
  walk.calls = open.reads->Calls();
  return walk;
}

// The records a batched walk asks its store for, given the groups it probes:
// each group's records that no earlier group probed, sorted and distinct, of
// the slots read through $MFT's extents.
[[nodiscard]] inline std::vector<ULONGLONG>
    AskedBy(const std::vector<std::vector<ULONGLONG>>& groups) {
  std::vector<ULONGLONG> asked;
  std::map<ULONGLONG, bool> known;
  for (const std::vector<ULONGLONG>& group : groups) {
    std::vector<ULONGLONG> fresh;
    for (const ULONGLONG record : group) {
      if (!known.contains(record)) {
        fresh.push_back(record);
      }
    }
    std::ranges::sort(fresh);
    fresh.erase(std::ranges::unique(fresh).begin(), fresh.end());
    for (const ULONGLONG record : fresh) {
      known.emplace(record, true);
      if (record >= first_mapped_slot) {
        asked.push_back(record);
      }
    }
  }
  return asked;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Io
