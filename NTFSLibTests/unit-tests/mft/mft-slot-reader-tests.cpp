#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "mft/slot-reader.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

template <Cache::Strategy S>
class AttrBase;

}  // namespace Attr
}  // namespace NTFS_BROWSER_NAMESPACE

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// Records of the fixture below: five fragments, the last one partial, so a
// window can cut a fragment and the last window is short.
constexpr ULONGLONG scan_record_count = 450;

// Records of the fixture the read counts use, and the clusters of each of its
// fragments: 17 fragments.
constexpr ULONGLONG large_scan_record_count = 4096;
constexpr ULONGLONG large_scan_fragment_clusters = 250;
constexpr size_t large_scan_fragments = 17;

// What a test double counts, and a range of the image it refuses to serve.
struct ReadCounters {
  size_t reads{0};
  ULONGLONG bytes{0};
  // [fail_begin, fail_end) in image bytes: a read overlapping it fails.
  LONGLONG fail_begin{-1};
  LONGLONG fail_end{-1};
};

// A MemoryDiskReader that counts its reads and fails the ones that overlap a
// range.
class CountingDiskReader final : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  CountingDiskReader(std::vector<BYTE> image,
                     std::shared_ptr<ReadCounters> counters)
      : inner_(std::move(image)), counters_(std::move(counters)) {}

  bool Open(std::wstring_view /*path*/) override { return false; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    counters_->reads++;
    counters_->bytes += dest.size();
    const LONGLONG end = addr.QuadPart + static_cast<LONGLONG>(dest.size());
    if (addr.QuadPart < counters_->fail_end && end > counters_->fail_begin) {
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

 private:
  Tests::MemoryDiskReader inner_;
  std::shared_ptr<ReadCounters> counters_;
};

template <Cache::Strategy S>
std::unique_ptr<NtfsVolume<S>>
    OpenScanVolume(ULONGLONG record_count,
                   const std::shared_ptr<ReadCounters>& counters,
                   const VolumeOptions& options = {},
                   ULONGLONG fragment_clusters =
                       Tests::fragmented_mft_scan_fragment_clusters) {
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<CountingDiskReader>(
          Tests::BuildFakeNtfsImageWithFragmentedMftScan(record_count,
                                                         fragment_clusters),
          counters),
      options);
  REQUIRE(volume->IsVolumeOK());
  REQUIRE(volume->GetRecordsCount() == record_count);
  return volume;
}

// What a test reads back from a FileRecord after parsing slot record.
struct Snapshot {
  bool parsed{false};
  bool attrs_parsed{false};
  std::optional<ULONGLONG> reference;
  WORD sequence{0};
  bool deleted{false};
  bool directory{false};
  bool extension{false};
  ULONGLONG base{0};
  std::wstring name;
  ULONGLONG size{0};

  bool operator==(const Snapshot&) const = default;
};

// Parses slot record through parse and snapshots the result.
template <Cache::Strategy S, typename Parse>
Snapshot Capture(Io::FileRecord<S>& file_record, ULONGLONG record,
                 const Parse& parse) {
  Snapshot snapshot;
  snapshot.parsed = parse(file_record, record);
  if (!snapshot.parsed) {
    return snapshot;
  }
  snapshot.attrs_parsed = file_record.ParseAttrs();
  snapshot.reference = file_record.GetFileReference();
  snapshot.sequence = file_record.GetSequenceNumber();
  snapshot.deleted = file_record.IsDeleted();
  snapshot.directory = file_record.IsDirectory();
  snapshot.extension = file_record.IsExtensionRecord();
  snapshot.base = file_record.GetBaseRecordReference();
  snapshot.name = std::wstring(file_record.GetFileName());
  snapshot.size = file_record.GetFileSize();
  return snapshot;
}

// The snapshot of slot record as FileRecord::ParseFileRecord() reads it.
template <Cache::Strategy S>
Snapshot ReferenceSnapshot(Io::FileRecord<S>& file_record, ULONGLONG record) {
  return Capture<S>(file_record, record,
                    [](Io::FileRecord<S>& parsed, ULONGLONG slot) {
                      return parsed.ParseFileRecord(slot);
                    });
}

// The snapshot of slot record as reader serves it.
template <Cache::Strategy S>
Snapshot ReaderSnapshot(Mft::SlotReader<S>& reader,
                        Io::FileRecord<S>& file_record, ULONGLONG record) {
  return Capture<S>(file_record, record,
                    [&reader](Io::FileRecord<S>& parsed, ULONGLONG slot) {
                      return reader.ParseFileRecord(parsed, slot);
                    });
}

// Prepares file_record to read the attributes the tree scan reads.
template <Cache::Strategy S>
void SetScanMask(Io::FileRecord<S>& file_record) {
  file_record.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
}

// Checks that, with blocks of block_bytes, reader gives every slot the
// snapshot ParseFileRecord() gives.
template <Cache::Strategy S>
void CheckSameAsRecordByRecord(size_t block_bytes) {
  const auto counters = std::make_shared<ReadCounters>();
  const auto volume = OpenScanVolume<S>(scan_record_count, counters);

  Io::FileRecord<S> expected(*volume);
  Io::FileRecord<S> actual(*volume);
  SetScanMask(expected);
  SetScanMask(actual);
  Mft::SlotReader<S> reader(*volume, block_bytes);

  for (ULONGLONG record = 0; record < scan_record_count; record++) {
    INFO("record " << record << ", block " << block_bytes << " bytes");
    CHECK(ReaderSnapshot(reader, actual, record) ==
          ReferenceSnapshot(expected, record));
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "SlotReader gives the same records as FileRecord::ParseFileRecord() "
    "whatever the block size",
    "[mft][slot-reader]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  // One record, then blocks that cut a fragment (100 records) anywhere, a
  // block that holds several fragments, and the default.
  const size_t block_bytes = GENERATE(
      size_t{1024}, size_t{3} * 1024, size_t{8} * 1024, size_t{37} * 1024,
      size_t{128} * 1024, size_t{1024} * 1024, Mft::default_slot_block_bytes);

  CheckSameAsRecordByRecord<S>(block_bytes);
}

TEMPLATE_TEST_CASE_SIG(
    "SlotReader gives the same records when a block cannot be read",
    "[mft][slot-reader]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto counters = std::make_shared<ReadCounters>();
  const auto volume = OpenScanVolume<S>(scan_record_count, counters);

  // The cluster of slot 120, in the middle of a block of 64 slots.
  constexpr ULONGLONG unreadable = 120;
  counters->fail_begin =
      static_cast<LONGLONG>(Tests::FragmentedMftScanSlotOffset(unreadable));
  counters->fail_end = counters->fail_begin + Tests::fake_file_record_size;

  Io::FileRecord<S> expected(*volume);
  Io::FileRecord<S> actual(*volume);
  SetScanMask(expected);
  SetScanMask(actual);
  Mft::SlotReader<S> reader(*volume, size_t{64} * 1024);

  for (ULONGLONG record = 0; record < scan_record_count; record++) {
    INFO("record " << record);
    const Snapshot want = ReferenceSnapshot(expected, record);
    CHECK(ReaderSnapshot(reader, actual, record) == want);
    if (record == unreadable) {
      CHECK_FALSE(want.parsed);
    }
  }
}

TEMPLATE_TEST_CASE_SIG("SlotReader reads only the slots Expect() allows",
                       "[mft][slot-reader]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto counters = std::make_shared<ReadCounters>();
  const auto volume = OpenScanVolume<S>(scan_record_count, counters);

  Io::FileRecord<S> expected(*volume);
  Io::FileRecord<S> actual(*volume);
  SetScanMask(expected);
  SetScanMask(actual);
  // Blocks of 16 slots, but the range holds 10.
  Mft::SlotReader<S> reader(*volume, size_t{16} * 1024);

  SECTION("a slot of the range is served from a block that stops at its end") {
    reader.Expect(100, 110);
    for (ULONGLONG record = 100; record < 110; record++) {
      INFO("record " << record);
      CHECK(ReaderSnapshot(reader, actual, record) ==
            ReferenceSnapshot(expected, record));
    }
  }

  SECTION("a slot outside the range is read alone") {
    reader.Expect(100, 110);
    CHECK(ReaderSnapshot(reader, actual, 120) ==
          ReferenceSnapshot(expected, 120));
    CHECK(ReaderSnapshot(reader, actual, 50) ==
          ReferenceSnapshot(expected, 50));
  }

  SECTION("a range that starts mid-block does not read before it") {
    reader.Expect(105, 110);
    const size_t reads_before = counters->reads;
    const ULONGLONG bytes_before = counters->bytes;
    const Snapshot served = ReaderSnapshot(reader, actual, 105);
    if constexpr (S == Cache::Strategy::NoCache) {
      // Slots 105 to 109 all lie in fragment 1: one read of five records.
      CHECK(counters->reads - reads_before == 1);
      CHECK(counters->bytes - bytes_before == 5 * Tests::fake_file_record_size);
    }
    CHECK(served == ReferenceSnapshot(expected, 105));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree over a fragmented $MFT agrees with a record by record scan",
    "[mft][slot-reader]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto counters = std::make_shared<ReadCounters>();
  const auto volume = OpenScanVolume<S>(scan_record_count, counters,
                                        VolumeOptions{.include_deleted = true});

  Io::FileRecord<S> reference(*volume);
  SetScanMask(reference);
  ULONGLONG unreadable = 0;
  ULONGLONG extensions = 0;
  ULONGLONG deleted = 0;
  ULONGLONG in_use = 0;
  for (ULONGLONG record = 0; record < scan_record_count; record++) {
    if (!reference.ParseFileRecord(record)) {
      unreadable++;
    } else if (reference.IsExtensionRecord()) {
      extensions++;
    } else if (reference.IsDeleted()) {
      deleted++;
    } else {
      in_use++;
    }
  }
  // The fixture holds every kind of slot.
  REQUIRE(unreadable > 0);
  REQUIRE(extensions > 0);
  REQUIRE(deleted > 0);

  const Mft::MftTree tree(*volume);

  const Mft::MftTree::ScanStats& stats = tree.Stats();
  CHECK(stats.slots == scan_record_count);
  CHECK(stats.unreadable == unreadable);
  CHECK(stats.extensions == extensions);
  CHECK(stats.deleted == deleted);
  CHECK(stats.in_use == in_use);
  CHECK(tree.Entries().size() == in_use + deleted);

  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    INFO("record " << entry.record);
    REQUIRE(reference.ParseFileRecord(entry.record));
    REQUIRE(reference.ParseAttrs());
    CHECK(entry.sequence == reference.GetSequenceNumber());
    CHECK(entry.in_use == !reference.IsDeleted());
    CHECK(entry.directory == reference.IsDirectory());
    const Attr::AttrBase<S>* data = reference.FindStream({});
    CHECK(entry.size ==
          (data != nullptr ? data->GetDataSize() : reference.GetFileSize()));
    if (entry.record >= static_cast<ULONGLONG>(Mft::Idx::User)) {
      REQUIRE_FALSE(entry.names.empty());
      CHECK(entry.names.front().name == std::wstring(reference.GetFileName()));
    }
  }
}

TEMPLATE_TEST_CASE_SIG("MftTree reads a large $MFT in far fewer disk reads",
                       "[mft][slot-reader]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto counters = std::make_shared<ReadCounters>();
  const auto volume = OpenScanVolume<S>(large_scan_record_count, counters, {},
                                        large_scan_fragment_clusters);

  // The reads a record by record scan makes.
  Io::FileRecord<S> reference(*volume);
  SetScanMask(reference);
  counters->reads = 0;
  for (ULONGLONG record = 0; record < large_scan_record_count; record++) {
    (void)reference.ParseFileRecord(record);
  }
  const size_t record_by_record_reads = counters->reads;

  // The volume is re-opened: FullCache would otherwise find the blocks.
  const auto fresh_counters = std::make_shared<ReadCounters>();
  const auto fresh_volume =
      OpenScanVolume<S>(large_scan_record_count, fresh_counters, {},
                        large_scan_fragment_clusters);
  fresh_counters->reads = 0;
  const Mft::MftTree tree(*fresh_volume);

  CHECK(tree.Stats().slots == large_scan_record_count);
  CHECK(tree.Stats().in_use > 0);
  if constexpr (S == Cache::Strategy::NoCache) {
    // One read per record before. Now one per fragment, plus the records
    // below Mft::Idx::User, which are still read alone.
    CHECK(record_by_record_reads >= large_scan_record_count - 16);
    CHECK(fresh_counters->reads <= large_scan_fragments + 16 + 4);
  } else {
    // FullCache reads 64 KiB blocks either way: the blocks of a fragment are
    // shared with its neighbour's, so the count may only grow by a little.
    CHECK(fresh_counters->reads <=
          record_by_record_reads + large_scan_fragments);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A SlotReader over a volume that did not open serves nothing",
    "[mft][slot-reader]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  // A volume with no boot sector has no record size to divide by.
  const NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(
      std::vector<BYTE>(size_t{64} * 1024, 0)));
  REQUIRE_FALSE(volume.IsVolumeOK());

  Mft::SlotReader<S> reader(volume);
  Io::FileRecord<S> record(volume);

  CHECK_FALSE(reader.ParseFileRecord(record, Tests::mft_tree_docs_idx));
}

TEMPLATE_TEST_CASE_SIG(
    "SlotReader::HeldSlot() gives the raw bytes of a slot its window served",
    "[mft][slot-reader]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto counters = std::make_shared<ReadCounters>();
  const auto volume = OpenScanVolume<S>(scan_record_count, counters);
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithFragmentedMftScan(scan_record_count);
  // Blocks of 16 slots.
  Mft::SlotReader<S> reader(*volume, size_t{16} * 1024);
  Io::FileRecord<S> record(*volume);

  // A slot past the system files, read through a window whatever it holds.
  constexpr ULONGLONG served = 100;
  static_cast<void>(reader.ParseFileRecord(record, served));
  const std::span<const BYTE> held = reader.HeldSlot(served);
  const auto offset =
      static_cast<std::ptrdiff_t>(Tests::FragmentedMftScanSlotOffset(served));
  CHECK(std::ranges::equal(
      held, std::span<const BYTE>{image}.subspan(
                static_cast<size_t>(offset), Tests::fake_file_record_size)));

  // Past the window, the slot is not held until a window holds it.
  CHECK(reader.HeldSlot(served + 16).empty());
  // Below Mft::Idx::User, a slot is read alone, never through a window.
  REQUIRE(reader.ParseFileRecord(record, 5));
  CHECK(reader.HeldSlot(5).empty());
}

namespace {

using HintKind = Tests::CountingDiskReader::Kind;
using HintCall = Tests::CountingDiskReader::Call;

// A volume over the fixture whose reader records every call, the prefetch
// hints included.
template <Cache::Strategy S>
struct HintedVolume {
  std::unique_ptr<NtfsVolume<S>> volume;
  Tests::CountingDiskReader* reader;
};

template <Cache::Strategy S>
HintedVolume<S> OpenHintedVolume() {
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(
          Tests::BuildFakeNtfsImageWithFragmentedMftScan(scan_record_count)));
  Tests::CountingDiskReader* const counting = reader.get();
  auto volume = std::make_unique<NtfsVolume<S>>(std::move(reader));
  REQUIRE(volume->IsVolumeOK());
  return {std::move(volume), counting};
}

// Whether call covers the bytes [offset, offset + length).
bool Covers(const HintCall& call, ULONGLONG offset, ULONGLONG length) {
  return call.offset <= offset && offset + length <= call.offset + call.length;
}

// Whether each read of calls lies in a range a hint announced before it.
bool EveryReadAnnouncedBefore(const std::vector<HintCall>& calls) {
  for (auto read = calls.begin(); read != calls.end(); ++read) {
    if (read->kind == HintKind::Read &&
        std::none_of(calls.begin(), read, [&read](const HintCall& hint) {
          return hint.kind == HintKind::Prefetch &&
                 Covers(hint, read->offset, read->length);
        })) {
      return false;
    }
  }
  return true;
}

// Whether a read or a hint of calls touches the record of slot.
bool TouchesSlot(const std::vector<HintCall>& calls, ULONGLONG slot) {
  const ULONGLONG first = Tests::FragmentedMftScanSlotOffset(slot);
  return std::ranges::any_of(calls, [first](const HintCall& call) {
    return (call.kind == HintKind::Read || call.kind == HintKind::Prefetch) &&
           call.offset < first + Tests::fake_file_record_size &&
           first < call.offset + call.length;
  });
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("SlotReader announces each window before it reads it",
                       "[mft][slot-reader][prefetch]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const HintedVolume<S> opened = OpenHintedVolume<S>();
  Io::FileRecord<S> record(*opened.volume);
  {
    // Blocks of 16 slots: the plan holds many windows, over five fragments.
    Mft::SlotReader<S> reader(*opened.volume, size_t{16} * 1024);
    reader.Expect(static_cast<ULONGLONG>(Mft::Idx::User), scan_record_count);
    opened.reader->Reset();
    for (ULONGLONG slot = static_cast<ULONGLONG>(Mft::Idx::User);
         slot < scan_record_count; slot++) {
      static_cast<void>(reader.ParseFileRecord(record, slot));
    }

    const std::vector<HintCall>& calls = opened.reader->Calls();
    REQUIRE(opened.reader->ReadCount() > 0);
    CHECK(EveryReadAnnouncedBefore(calls));
    // The windows after the first one are announced before the first read.
    const auto first_read =
        std::ranges::find(calls, HintKind::Read, &HintCall::kind);
    CHECK(std::count_if(calls.begin(), first_read, [](const HintCall& call) {
            return call.kind == HintKind::Prefetch;
          }) > 1);
  }
  // The reader withdraws its hints when it goes.
  CHECK(opened.reader->Calls().back().kind == HintKind::CancelPrefetch);
}

TEST_CASE(
    "SlotReader cuts the windows of a range from its first slot, whatever "
    "slots are asked for",
    "[mft][slot-reader][prefetch]") {
  const HintedVolume<Cache::Strategy::NoCache> opened =
      OpenHintedVolume<Cache::Strategy::NoCache>();
  Io::FileRecord<Cache::Strategy::NoCache> record(*opened.volume);
  // Windows of 16 slots: [20, 36), [36, 52), ... and [84, 90).
  Mft::SlotReader<Cache::Strategy::NoCache> reader(*opened.volume,
                                                   size_t{16} * 1024);
  reader.Expect(20, 90);
  opened.reader->Reset();

  static_cast<void>(reader.ParseFileRecord(record, 41));

  const std::vector<HintCall>& calls = opened.reader->Calls();
  CHECK(opened.reader->ReadCount() == 1);
  CHECK(std::ranges::any_of(calls, [](const HintCall& call) {
    return call.kind == HintKind::Read &&
           call.offset == Tests::FragmentedMftScanSlotOffset(36) &&
           call.length == size_t{16} * 1024;
  }));
  // The short last window is announced too, and nothing past the range.
  CHECK(std::ranges::any_of(calls, [](const HintCall& call) {
    return call.kind == HintKind::Prefetch &&
           call.offset == Tests::FragmentedMftScanSlotOffset(84) &&
           call.length == size_t{6} * 1024;
  }));
  CHECK_FALSE(TouchesSlot(calls, 90));
  CHECK_FALSE(TouchesSlot(calls, 19));
}

TEMPLATE_TEST_CASE_SIG(
    "SlotReader reads and announces only the slots of its plan",
    "[mft][slot-reader][prefetch]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const HintedVolume<S> opened = OpenHintedVolume<S>();
  Io::FileRecord<S> expected(*opened.volume);
  Io::FileRecord<S> actual(*opened.volume);
  SetScanMask(expected);
  SetScanMask(actual);
  Mft::SlotReader<S> reader(*opened.volume, size_t{16} * 1024);
  const std::vector<Mft::SlotRange> plan{{.first = 16, .last = 40},
                                         {.first = 200, .last = 210},
                                         {.first = 400, .last = 450}};
  reader.Plan(plan);
  opened.reader->Reset();

  for (const Mft::SlotRange& range : plan) {
    for (ULONGLONG slot = range.first; slot < range.last; slot++) {
      static_cast<void>(reader.ParseFileRecord(actual, slot));
    }
  }

  const std::vector<HintCall> calls = opened.reader->Calls();
  CHECK(EveryReadAnnouncedBefore(calls));
  if constexpr (S == Cache::Strategy::NoCache) {
    // FullCache reads and announces whole blocks of 64 KiB.
    for (const ULONGLONG outside :
         {ULONGLONG{60}, ULONGLONG{150}, ULONGLONG{300}, ULONGLONG{399}}) {
      INFO("slot " << outside);
      CHECK_FALSE(TouchesSlot(calls, outside));
    }
  }
  // The records served are those FileRecord reads itself.
  for (const Mft::SlotRange& range : plan) {
    for (ULONGLONG slot = range.first; slot < range.last; slot += 7) {
      INFO("slot " << slot);
      CHECK(ReaderSnapshot(reader, actual, slot) ==
            ReferenceSnapshot(expected, slot));
    }
  }
}

TEST_CASE("NtfsVolume::PrefetchMftSlots announces one range per fragment",
          "[mft][slot-reader][prefetch]") {
  const HintedVolume<Cache::Strategy::NoCache> opened =
      OpenHintedVolume<Cache::Strategy::NoCache>();
  opened.reader->Reset();

  // Slots 90 to 109 span the end of the first fragment and the next one.
  opened.volume->PrefetchMftSlots(90, 20);
  const std::vector<HintCall>& calls = opened.reader->Calls();
  REQUIRE(calls.size() == 2);
  CHECK(calls.at(0).kind == HintKind::Prefetch);
  CHECK(calls.at(0).offset == Tests::FragmentedMftScanSlotOffset(90));
  CHECK(calls.at(0).length == size_t{10} * 1024);
  CHECK(calls.at(1).kind == HintKind::Prefetch);
  CHECK(calls.at(1).offset == Tests::FragmentedMftScanSlotOffset(100));
  CHECK(calls.at(1).length == size_t{10} * 1024);

  // The records below Mft::Idx::User sit at a fixed address: no hint.
  opened.reader->Reset();
  opened.volume->PrefetchMftSlots(5, 4);
  opened.volume->PrefetchMftSlots(20, 0);
  CHECK(opened.reader->Calls().empty());
}
