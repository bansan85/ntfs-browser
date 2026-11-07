#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/alloc/cluster-account-support.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/mft/mft-tree-state-support.h>
#include <ntfs-browser/test/usn/usn-journal-record.h>
#include <ntfs-browser/test/usn/usn-test-support.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "knob-effect-support.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Account = NTFS_BROWSER_NAMESPACE::Tests::AccountSupport;
namespace AllocSupport = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
namespace TreeSupport = NTFS_BROWSER_NAMESPACE::Tests::MftTreeSupport;
namespace Usn = NTFS_BROWSER_NAMESPACE::Usn;
namespace UsnSupport = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using namespace Tests::KnobEffect;

namespace {

// The smallest block, bitmap window or read a profile may set.
constexpr std::uint64_t min_bytes = 4096;

// A block of the cache larger than the default: its former size.
constexpr std::uint64_t large_block = 65536;

// Clusters of the volume whose bitmap spans two windows of 4096 bytes.
constexpr ULONGLONG two_window_clusters = 65536;

// First cluster of the data of the content check fixture, and the clusters in
// each of its two files.
constexpr ULONGLONG data_lcn = 100;
constexpr ULONGLONG file_clusters = 16;

// The data of the check fixture: two files of 16 clusters, four clusters
// apart.
struct CheckScan {
  std::vector<Call> data_calls;
};

std::vector<BYTE> CheckImage() {
  Account::Volume volume;
  volume.bitmap_options.volume_clusters = 400;
  constexpr ULONGLONG bytes = file_clusters * TestFakeImage::fake_cluster_size;
  volume.Put(22, Account::Freed(
                     100, Account::Stream({{data_lcn, file_clusters}}, bytes)));
  volume.Put(
      23, Account::Freed(
              101, Account::Stream({{data_lcn + 20, file_clusters}}, bytes)));
  return volume.Build();
}

CheckScan ScanCheck(const VolumeOptions& options) {
  const TreeSupport::ScannedVolume<Cache::Strategy::NoCache> scanned(
      CheckImage(), options,
      Account::Scan(Alloc::ClusterAccountLevel::Counts,
                    Alloc::DataCheckMode::Full));
  CheckScan scan;
  scan.data_calls = Region(scanned.opened.counter->Calls(),
                           data_lcn * TestFakeImage::fake_cluster_size,
                           (data_lcn + 36) * TestFakeImage::fake_cluster_size);
  return scan;
}

// A reader over memory that notes the reads other threads than the one that
// built it make: the read ahead of a decorator.
struct WorkerReads {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::pair<ULONGLONG, size_t>> reads;
  std::thread::id caller{std::this_thread::get_id()};
};

class WorkerLoggingReader final : public IDiskReader {
 public:
  WorkerLoggingReader(std::vector<BYTE> image,
                      std::shared_ptr<WorkerReads> seen)
      : inner_(std::move(image)), seen_(std::move(seen)) {}

  bool Open(std::wstring_view /*path*/) override { return true; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    const bool read = inner_.ReadInto(addr, dest);
    if (std::this_thread::get_id() != seen_->caller) {
      const std::scoped_lock lock{seen_->mutex};
      seen_->reads.emplace_back(static_cast<ULONGLONG>(addr.QuadPart),
                                dest.size());
      seen_->changed.notify_all();
    }
    return read;
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    return inner_.Size();
  }

 private:
  TestDiskReaders::MemoryDiskReader inner_;
  std::shared_ptr<WorkerReads> seen_;
};

// The reads a worker made, as (offset, length).
using WorkerReadList = std::vector<std::pair<ULONGLONG, size_t>>;

// Waits until the worker has made expected reads from base on.
void WaitForWorkerReads(WorkerReads& seen, ULONGLONG base, size_t expected) {
  std::unique_lock lock{seen.mutex};
  const auto from_base = [&] {
    return std::ranges::count_if(
        seen.reads, [base](const auto& read) { return read.first >= base; });
  };
  // A bound on a bug, never a measure of speed.
  REQUIRE(seen.changed.wait_for(lock, std::chrono::seconds{30}, [&] {
    return static_cast<size_t>(from_base()) >= expected;
  }));
}

// Two forward reads of 1000 bytes of the volume, from base.
void ReadForwardTwice(const NtfsVolume<Cache::Strategy::NoCache>& volume,
                      ULONGLONG base) {
  std::vector<BYTE> dest(1000);
  LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(base)};
  for (int read = 0; read < 2; read++) {
    REQUIRE(volume.ReadInto(addr, dest));
    addr.QuadPart += static_cast<LONGLONG>(dest.size());
  }
}

// The reads the read ahead of a volume made from base on, after two forward
// reads of the caller there, once the worker has made expected of them.
WorkerReadList ReadAheadFrom(const VolumeOptions& options, size_t expected) {
  // The volume, then room for the stream far from what opening it reads.
  std::vector<BYTE> image = TestFakeImage::BuildFakeNtfsImageWithMftTree();
  const ULONGLONG base = image.size();
  image.resize(base + (1U << 20U));
  const auto seen = std::make_shared<WorkerReads>();
  {
    NtfsVolume<Cache::Strategy::NoCache> volume(
        std::make_unique<WorkerLoggingReader>(image, seen), options);
    REQUIRE(volume.IsVolumeOK());
    ReadForwardTwice(volume, base);
    WaitForWorkerReads(*seen, base, expected);
  }
  // The volume is gone and its worker joined: the list is final.
  WorkerReadList reads;
  std::ranges::copy_if(seen->reads, std::back_inserter(reads),
                       [base](const auto& read) { return read.first >= base; });
  return reads;
}

ULONGLONG ReadAheadBase() {
  return TestFakeImage::BuildFakeNtfsImageWithMftTree().size();
}

}  // namespace

TEST_CASE("cache.block_bytes sets the block FullCache reads and holds",
          "[tuning][knob-effect][cache]") {
  const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithMftTree();
  REQUIRE(image.size() >= large_block);
  const std::uint64_t block =
      GENERATE(min_bytes, std::uint64_t{16384}, large_block);
  INFO("block " << block);

  auto reader = std::make_unique<TestDiskReaders::CountingDiskReader>(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(image));
  TestDiskReaders::CountingDiskReader& reads = *reader;
  const NtfsVolume<Cache::Strategy::FullCache> volume(
      std::move(reader), Tuned({}, {{"cache.block_bytes", block}}));
  REQUIRE(volume.IsVolumeOK());
  // Opening cached the blocks of the boot sector and of $MFT.
  volume.MinifyCache();
  reads.Reset();

  LARGE_INTEGER addr{.QuadPart = 20000};
  const auto first = volume.Read(addr, 100);
  REQUIRE(first.has_value());
  // The block that holds the address, whole, and nothing else.
  REQUIRE(reads.Calls().size() == 1);
  CHECK(reads.Calls().front().length == block);
  CHECK(reads.Calls().front().offset == 20000 - (20000 % block));
  CHECK(volume.GetCacheStats().held_bytes == block);

  // Another byte of the same block is a hit; the next block is a read.
  addr.QuadPart = 20100;
  REQUIRE(volume.Read(addr, 100).has_value());
  CHECK(reads.Calls().size() == 1);
  const std::uint64_t next = 20000 - (20000 % block) + block;
  if (next + block <= image.size()) {
    addr.QuadPart = static_cast<LONGLONG>(next);
    REQUIRE(volume.Read(addr, 100).has_value());
    CHECK(reads.Calls().size() == 2);
  }
}

TEST_CASE("cache.max_run_bytes sets the most one read of the cache covers",
          "[tuning][knob-effect][cache]") {
  const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithMftTree();
  constexpr std::uint64_t blocks = 8;
  constexpr std::uint64_t first = 4 * min_bytes;
  REQUIRE(image.size() >= first + (blocks * min_bytes));
  const std::uint64_t run = GENERATE(min_bytes, 2 * min_bytes, 3 * min_bytes,
                                     blocks * min_bytes, 16 * min_bytes);
  INFO("run " << run);

  auto reader = std::make_unique<TestDiskReaders::CountingDiskReader>(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(image));
  TestDiskReaders::CountingDiskReader& reads = *reader;
  const NtfsVolume<Cache::Strategy::FullCache> volume(
      std::move(reader), Tuned({}, {{"cache.max_run_bytes", run}}));
  REQUIRE(volume.IsVolumeOK());
  volume.MinifyCache();
  reads.Reset();

  LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(first)};
  const auto view = volume.Read(addr, static_cast<DWORD>(blocks * min_bytes));
  REQUIRE(view.has_value());

  // Whole runs from the start of the range, the last one shorter.
  const std::uint64_t total = blocks * min_bytes;
  const std::uint64_t most = std::min(run, total);
  const std::uint64_t reads_expected = (total + most - 1) / most;
  const std::vector<Call> calls =
      Select(reads.Calls(), Kind::Read, [](const Call&) { return true; });
  REQUIRE(calls.size() == reads_expected);
  std::uint64_t at = first;
  for (const Call& call : calls) {
    CHECK(call.offset == at);
    CHECK(call.length <= most);
    at += call.length;
  }
  CHECK(at == first + total);
}

TEST_CASE("cache.max_bytes sets the memory FullCache keeps once a view ends",
          "[tuning][knob-effect][cache]") {
  const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithMftTree();
  const std::uint64_t ceiling = GENERATE(std::uint64_t{0}, large_block);

  const NtfsVolume<Cache::Strategy::FullCache> volume(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(image),
      Tuned({}, {{"cache.max_bytes", ceiling}}));
  REQUIRE(volume.IsVolumeOK());
  volume.MinifyCache();
  LARGE_INTEGER addr{.QuadPart = 20000};
  {
    const auto view = volume.Read(addr, 100);
    REQUIRE(view.has_value());
    CHECK(volume.GetCacheStats().in_use_bytes > 0);
  }

  // A ceiling of 0 frees a block as its last view goes.
  INFO("ceiling " << ceiling);
  CHECK((volume.GetCacheStats().held_bytes == 0) == (ceiling == 0));
}

TEST_CASE("alloc.bitmap_window_bytes sets the size of the bitmap reads",
          "[tuning][knob-effect][alloc]") {
  std::vector<BYTE> image = TestFakeImage::BuildFakeNtfsImageWithMftTree();
  const std::vector<BYTE> bits = TestFakeImage::AllocatedBitmapFor(image, {});
  TestFakeImage::PutClusterBitmap(image, bits,
                                  {.volume_clusters = two_window_clusters});
  const ULONGLONG bitmap_from =
      two_window_clusters * TestFakeImage::fake_cluster_size;

  const auto summary = [&](const VolumeOptions& options) {
    const auto opened =
        AllocSupport::OpenVolume<Cache::Strategy::NoCache>(image, options);
    opened.counter->Reset();
    const auto space = opened.volume->GetSpaceSummary();
    REQUIRE(space.has_value());
    return std::make_pair(*space, Select(opened.counter->Calls(), Kind::Read,
                                         [bitmap_from](const Call& call) {
                                           return call.offset >= bitmap_from &&
                                                  call.length >= min_bytes;
                                         }));
  };

  const auto [base_space, base_reads] = summary({});

  const auto [tuned_space, tuned_reads] =
      summary(Tuned({}, {{"alloc.bitmap_window_bytes", min_bytes}}));

  // The default window holds the 8192 bytes of this bitmap; 4096 takes two.
  REQUIRE(base_reads.size() == 1);
  REQUIRE(tuned_reads.size() == 2);
  for (const Call& read : tuned_reads) {
    CHECK(read.length == min_bytes);
  }
  CHECK(tuned_space.total_clusters == base_space.total_clusters);
  CHECK(tuned_space.free_clusters == base_space.free_clusters);
  CHECK(tuned_space.unknown_clusters == base_space.unknown_clusters);
  CHECK(base_space.free_clusters > 0);
}

TEST_CASE("check.max_read_bytes sets the size of one read of the content check",
          "[tuning][knob-effect][check]") {
  const std::uint64_t read_bytes = GENERATE(min_bytes, std::uint64_t{8192});
  INFO("read " << read_bytes);

  const CheckScan base = ScanCheck(AllocSupport::DeletedOptions());
  const CheckScan tuned = ScanCheck(Tuned(
      AllocSupport::DeletedOptions(), {{"check.max_read_bytes", read_bytes}}));

  // The default merges both files into one read, over the gap between them.
  CHECK(Count(base.data_calls, Kind::Read) == 1);
  CHECK(LongestRead(tuned.data_calls) <= read_bytes);
  CHECK(Count(tuned.data_calls, Kind::Read) ==
        (2 * file_clusters * TestFakeImage::fake_cluster_size) / read_bytes);
}

TEST_CASE("check.max_gap_bytes sets the hole a read of the content check spans",
          "[tuning][knob-effect][check]") {
  const CheckScan base = ScanCheck(AllocSupport::DeletedOptions());
  const CheckScan apart = ScanCheck(
      Tuned(AllocSupport::DeletedOptions(), {{"check.max_gap_bytes", 0}}));

  // The files lie four clusters apart: no hole allowed reads each by itself.
  CHECK(Count(base.data_calls, Kind::Read) == 1);
  CHECK(Count(apart.data_calls, Kind::Read) == 2);
  CHECK(LongestRead(apart.data_calls) ==
        file_clusters * TestFakeImage::fake_cluster_size);
  CHECK(LongestRead(base.data_calls) >
        2 * file_clusters * TestFakeImage::fake_cluster_size);
}

TEST_CASE("check.max_announced_reads bounds the reads announced ahead",
          "[tuning][knob-effect][check]") {
  const VolumeOptions small_reads = Tuned(
      AllocSupport::DeletedOptions(), {{"check.max_read_bytes", min_bytes}});
  const CheckScan wide = ScanCheck(small_reads);
  const CheckScan narrow =
      ScanCheck(Tuned(small_reads, {{"check.max_announced_reads", 2}}));

  CHECK(Count(narrow.data_calls, Kind::Read) ==
        Count(wide.data_calls, Kind::Read));
  CHECK(MaxOutstanding(wide.data_calls).ranges > 2);
  CHECK(MaxOutstanding(narrow.data_calls).ranges <= 2);
}

TEST_CASE("check.max_announced_bytes bounds the bytes announced ahead",
          "[tuning][knob-effect][check]") {
  const VolumeOptions small_reads = Tuned(
      AllocSupport::DeletedOptions(), {{"check.max_read_bytes", min_bytes}});
  const CheckScan wide = ScanCheck(small_reads);
  const CheckScan narrow = ScanCheck(
      Tuned(small_reads, {{"check.max_announced_bytes", 2 * min_bytes}}));

  CHECK(MaxOutstanding(wide.data_calls).bytes > 2 * min_bytes);
  CHECK(MaxOutstanding(narrow.data_calls).bytes <= 2 * min_bytes);
}

TEST_CASE("usn.read_chunk_bytes sets the size of the reads of $J",
          "[tuning][knob-effect][usn]") {
  // Nine MiB of journal.
  const TestFakeImage::FakeUsnOptions journal{
      .raw_stream = std::vector<BYTE>(std::size_t{9} << 20U, BYTE{0})};
  const std::vector<BYTE> image = UsnSupport::JournalImage(journal);
  const ULONGLONG journal_from =
      TestFakeImage::BuildFakeNtfsImageWithFreedFileTree().size();
  const std::uint64_t chunk =
      GENERATE(std::uint64_t{262144}, std::uint64_t{2} << 20U);
  INFO("chunk " << chunk);

  const auto loader_reads = [&](const VolumeOptions& options) {
    const auto opened =
        UsnSupport::OpenVolume<Cache::Strategy::NoCache>(image, options);
    opened.counter->Reset();
    REQUIRE(opened.volume->GetUsnJournal() != nullptr);
    return Select(opened.counter->Calls(), Kind::Read,
                  [journal_from](const Call& call) {
                    return call.offset >= journal_from;
                  });
  };

  const std::vector<Call> base = loader_reads(UsnSupport::RecoverOptions());
  const std::vector<Call> tuned = loader_reads(
      Tuned(UsnSupport::RecoverOptions(), {{"usn.read_chunk_bytes", chunk}}));

  CHECK(base.size() == 9);
  CHECK(LongestRead(tuned) == chunk);
  CHECK(tuned.size() == ((std::uint64_t{9} << 20U) + chunk - 1) / chunk);
}

TEST_CASE("usn.read_chunk_bytes leaves the records of the journal as they were",
          "[tuning][knob-effect][usn]") {
  const TestFakeImage::FakeUsnOptions journal{
      .records = {UsnSupport::MakeRecord({.name = L"a.txt", .record = 40}),
                  UsnSupport::MakeRecord({.name = L"b.txt", .record = 41})}};
  const std::vector<BYTE> image = UsnSupport::JournalImage(journal);

  const auto names = [&](const VolumeOptions& options) {
    const auto opened =
        UsnSupport::OpenVolume<Cache::Strategy::NoCache>(image, options);
    const Usn::Journal* const loaded = opened.volume->GetUsnJournal();
    REQUIRE(loaded != nullptr);
    std::vector<std::wstring> found;
    for (const Usn::Record& record : loaded->records) {
      found.push_back(record.name);
    }
    return found;
  };

  const std::vector<std::wstring> base = names(UsnSupport::RecoverOptions());
  REQUIRE(base.size() == 2);
  CHECK(names(Tuned(UsnSupport::RecoverOptions(),
                    {{"usn.read_chunk_bytes", min_bytes}})) == base);
}

TEST_CASE("mft.journal_piece_bytes sets the ranges announced for $J",
          "[tuning][knob-effect][usn]") {
  const TestFakeImage::FakeUsnOptions journal{
      .raw_stream = std::vector<BYTE>(std::size_t{9} << 20U, BYTE{0})};
  const std::vector<BYTE> image = UsnSupport::JournalImage(journal);
  const ULONGLONG journal_from =
      TestFakeImage::BuildFakeNtfsImageWithFreedFileTree().size();
  const std::uint64_t piece = std::uint64_t{1} << 20U;

  const auto hints = [&](const VolumeOptions& options) {
    const auto opened =
        UsnSupport::OpenVolume<Cache::Strategy::NoCache>(image, options);
    const Mft::MftTree tree(*opened.volume);
    return Select(opened.counter->Calls(), Kind::Prefetch,
                  [journal_from](const Call& call) {
                    return call.offset >= journal_from;
                  });
  };

  const std::vector<Call> base = hints(UsnSupport::RecoverOptions());
  const std::vector<Call> tuned = hints(Tuned(
      UsnSupport::RecoverOptions(), {{"mft.journal_piece_bytes", piece}}));

  CHECK(base.size() == 3);
  CHECK(tuned.size() == 9);
  for (const Call& hint : tuned) {
    CHECK(hint.length <= piece);
  }
}

TEST_CASE("readahead.chunk_bytes sets the size of the reads ahead",
          "[tuning][knob-effect][readahead]") {
  const std::uint64_t chunk = GENERATE(min_bytes, std::uint64_t{8192});
  INFO("chunk " << chunk);

  VolumeOptions options = Tuned({}, {{"readahead.chunk_bytes", chunk}});
  const auto reads = ReadAheadFrom(options, 2);

  REQUIRE(reads.size() == 2);
  const ULONGLONG start = ReadAheadBase() + 2000;
  for (size_t i = 0; i < reads.size(); i++) {
    CHECK(reads.at(i).second == chunk);
    CHECK(reads.at(i).first == start + (i * chunk));
  }
}

TEST_CASE("readahead.window_chunks sets the chunks kept read ahead",
          "[tuning][knob-effect][readahead]") {
  const std::uint64_t chunks = GENERATE(std::uint64_t{1}, std::uint64_t{3});
  INFO("chunks " << chunks);

  const VolumeOptions options =
      Tuned({}, {{"readahead.chunk_bytes", min_bytes},
                 {"readahead.window_chunks", chunks}});
  const auto reads = ReadAheadFrom(options, static_cast<size_t>(chunks));

  CHECK(reads.size() == chunks);
}

TEST_CASE("The read ahead makes the chunks of the default window by default",
          "[tuning][knob-effect][readahead]") {
  const VolumeOptions options =
      Tuned({}, {{"readahead.chunk_bytes", min_bytes}});

  CHECK(ReadAheadFrom(options, 2).size() == 2);
}
