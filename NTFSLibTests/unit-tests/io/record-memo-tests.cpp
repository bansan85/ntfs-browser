#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "directory-read-ahead-support.h"
#include "io/record-memo.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace ReadAhead = NTFS_BROWSER_NAMESPACE::Tests::ReadAhead;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// The size of a file record of most volumes.
constexpr std::size_t record_size = 1024;

// Offsets of the fields of a file record header that a probe reads.
constexpr std::size_t offset_of_us_at = 4;
constexpr std::size_t size_of_us_at = 6;
constexpr std::size_t seq_no_at = 0x10;
constexpr std::size_t flags_at = 0x16;

// Where the update sequence array of the records below starts: where NTFS puts
// it.
constexpr std::size_t array_at = 0x30;

// The update sequence number of the records below.
constexpr WORD usn = 0x7777;

void SetWord(std::vector<BYTE>& bytes, std::size_t at, WORD value) {
  std::memcpy(&bytes.at(at), &value, sizeof(value));
}

// A record of record_size bytes that a probe accepts: the signature, an array
// of one word per sector, and the update sequence number at the end of each
// sector. Every other byte is filler that a probe never reads.
std::vector<BYTE> Record(WORD seq_no, WORD flags,
                         std::size_t size = record_size, BYTE filler = 0xAB) {
  std::vector<BYTE> bytes(size, filler);
  std::memcpy(bytes.data(), "FILE", 4);
  const std::size_t sectors = size / 512;
  SetWord(bytes, offset_of_us_at, array_at);
  SetWord(bytes, size_of_us_at, static_cast<WORD>(1 + sectors));
  SetWord(bytes, seq_no_at, seq_no);
  SetWord(bytes, flags_at, flags);
  SetWord(bytes, array_at, usn);
  for (std::size_t sector = 0; sector < sectors; sector++) {
    // The words the sectors held before NTFS wrote the number over them.
    SetWord(bytes, array_at + (2 * (1 + sector)),
            static_cast<WORD>(0x1111 * (1 + sector)));
    SetWord(bytes, ((sector + 1) * 512) - 2, usn);
  }
  return bytes;
}

// Records of one run, back to back.
std::vector<BYTE> Run(const std::vector<std::vector<BYTE>>& records) {
  std::vector<BYTE> bytes;
  for (const std::vector<BYTE>& record : records) {
    bytes.insert(bytes.end(), record.begin(), record.end());
  }
  return bytes;
}

}  // namespace

TEST_CASE("A record memo gives back what a probe reads of a record",
          "[io][record-memo]") {
  Io::RecordMemo memo(64, record_size);
  CHECK(memo.Enabled());
  CHECK(memo.Find(20).empty());

  const std::vector<BYTE> first = Record(3, 1);
  const std::vector<BYTE> second = Record(9, 0);
  memo.Put(20, Run({first, second}));

  const auto rebuilt = memo.Find(20);
  REQUIRE(rebuilt.size() == record_size);
  // What a probe reads is the same, and no more of it is kept.
  for (const std::size_t at :
       {std::size_t{0}, offset_of_us_at, size_of_us_at, seq_no_at, flags_at,
        array_at, array_at + 2, array_at + 4, std::size_t{510},
        std::size_t{1022}}) {
    INFO("offset " << at);
    CHECK(rebuilt[at] == first.at(at));
    CHECK(rebuilt[at + 1] == first.at(at + 1));
  }
  CHECK(rebuilt[0x20] == 0);
  CHECK(rebuilt[record_size / 2] == 0);
  CHECK(memo.Find(21).front() == 'F');
  const WORD seq = static_cast<WORD>(memo.Find(21)[seq_no_at] |
                                     (memo.Find(21)[seq_no_at + 1] << 8U));
  CHECK(seq == 9);
  CHECK(memo.Find(19).empty());
  CHECK(memo.Find(22).empty());
}

TEST_CASE("A record memo holds nothing when it has no room or no record size",
          "[io][record-memo]") {
  const std::vector<BYTE> run = Run({Record(1, 1)});

  Io::RecordMemo none(0, record_size);
  CHECK_FALSE(none.Enabled());
  none.Put(5, run);
  CHECK(none.Find(5).empty());
  CHECK(none.TableBytes() == 0);

  // A size that is no whole number of sectors, or past the largest record.
  for (const std::size_t size : {std::size_t{0}, std::size_t{100},
                                 std::size_t{1000}, std::size_t{8192}}) {
    INFO("record size " << size);
    Io::RecordMemo odd(64, size);
    CHECK_FALSE(odd.Enabled());
  }
  // The sizes a volume has.
  for (const std::size_t size : {std::size_t{512}, std::size_t{1024},
                                 std::size_t{2048}, std::size_t{4096}}) {
    INFO("record size " << size);
    Io::RecordMemo memo(64, size);
    CHECK(memo.Enabled());
    memo.Put(7, Record(2, 1, size));
    CHECK(memo.Find(7).size() == size);
  }
}

TEST_CASE("A record memo takes 32 bytes a record of 1 KiB",
          "[io][record-memo]") {
  CHECK(Io::RecordMemo(65536, record_size).TableBytes() == 65536 * 32U);
  CHECK(Io::RecordMemo(10, 4096).TableBytes() == 10 * 56U);
}

TEST_CASE("A record memo drops a record that a probe judges by other bytes",
          "[io][record-memo]") {
  Io::RecordMemo memo(64, record_size);
  memo.Put(1, Record(1, 1));
  REQUIRE_FALSE(memo.Find(1).empty());

  SECTION("not a FILE record") {
    std::vector<BYTE> blank(record_size, 0);
    memo.Put(1, blank);
    CHECK(memo.Find(1).empty());
  }
  SECTION("an array that starts inside the header") {
    std::vector<BYTE> forged = Record(1, 1);
    SetWord(forged, offset_of_us_at, 0x10);
    memo.Put(1, forged);
    CHECK(memo.Find(1).empty());
  }
  SECTION("an array that reaches the end of the first sector") {
    std::vector<BYTE> forged = Record(1, 1);
    SetWord(forged, offset_of_us_at, 508);
    memo.Put(1, forged);
    CHECK(memo.Find(1).empty());
  }
  SECTION("a record that is only partly there") {
    const std::vector<BYTE> whole = Record(4, 1);
    memo.Put(2, std::vector<BYTE>(whole.begin(), whole.end() - 1));
    CHECK(memo.Find(2).empty());
  }
}

TEST_CASE("A record memo keeps the newest record of a place",
          "[io][record-memo]") {
  Io::RecordMemo memo(8, record_size);
  memo.Put(3, Record(1, 1));
  memo.Put(11, Record(2, 1));
  CHECK(memo.Find(3).empty());
  CHECK_FALSE(memo.Find(11).empty());

  // A record that is not kept does not drop the one of another slot.
  std::vector<BYTE> blank(record_size, 0);
  memo.Put(19, blank);
  CHECK_FALSE(memo.Find(11).empty());
  CHECK(memo.Find(19).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "A probe judges a record rebuilt by the memo as it judges the record",
    "[io][record-memo]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::size_t compared = 0;
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    for (const VolumeOptions& options : ReadAhead::AllOptions()) {
      std::vector<BYTE> image = fixture.image;
      const ReadAhead::MftSpan mft = ReadAhead::MftSpanOf(image);
      const std::size_t slots = (mft.end - mft.first) / record_size;
      // Damage the sector ends of some records: a failed update sequence check
      // is judged by the same bytes, and the memo keeps them as they are.
      for (std::size_t slot = 3; slot < slots; slot += 7) {
        image.at(mft.first + (slot * record_size) + 1022) ^= 0x55;
      }

      NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(image),
                           options);
      REQUIRE(volume.IsVolumeOK());
      REQUIRE(volume.GetFileRecordSize() == record_size);
      Io::FileRecord<S> full(volume);
      Io::FileRecord<S> rebuilt(volume);
      Io::RecordMemo memo(slots, record_size);
      const std::span<const BYTE> mft_bytes(image.data() + mft.first,
                                            slots * record_size);
      memo.Put(0, mft_bytes);

      for (std::size_t slot = 0; slot < slots; slot++) {
        const std::span<const BYTE> original =
            mft_bytes.subspan(slot * record_size, record_size);
        const std::span<const BYTE> kept = memo.Find(slot);
        if (kept.empty()) {
          continue;
        }
        INFO(fixture.name << ", slot " << slot << ", include_deleted "
                          << options.include_deleted << ", recover_errors "
                          << options.recover_errors);
        (void)Tests::TakeCapturedLog();
        const bool full_parsed = full.ParseFileRecord(slot, original);
        const std::string full_log = Tests::TakeCapturedLog();
        const bool rebuilt_parsed = rebuilt.ParseFileRecord(slot, kept);
        const std::string rebuilt_log = Tests::TakeCapturedLog();

        CHECK(rebuilt_parsed == full_parsed);
        CHECK(rebuilt_log == full_log);
        CHECK(rebuilt.HasFailedFixup() == full.HasFailedFixup());
        if (full_parsed) {
          CHECK(rebuilt.IsDeleted() == full.IsDeleted());
          CHECK(rebuilt.GetSequenceNumber() == full.GetSequenceNumber());
        }
        compared++;
      }
    }
  }
  CHECK(compared > 1000);
}
