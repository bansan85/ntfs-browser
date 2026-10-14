#include <ntfs-browser/win-types.h>

#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "nonresident-index-support.h"
#include "test-log-sink.h"

namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;
using Catch::Matchers::ContainsSubstring;

namespace {

// Serves an image like MemoryDiskReader, but fails or throws on any read
// that touches [fault_begin, fault_end).
class FaultyDiskReader final : public NtfsBrowser::IDiskReader {
 public:
  enum class Fault { Fail, Throw };

  FaultyDiskReader(std::vector<BYTE> image, ULONGLONG fault_begin,
                   ULONGLONG fault_end, Fault fault)
      : inner_(std::move(image)),
        fault_begin_(fault_begin),
        fault_end_(fault_end),
        fault_(fault) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  // Faults stay off until Arm(), so the volume can open and parse first.
  void Arm() { armed_ = true; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    const auto begin = static_cast<ULONGLONG>(addr.QuadPart);
    const ULONGLONG end = begin + dest.size();
    if (armed_ && begin < fault_end_ && fault_begin_ < end) {
      if (fault_ == Fault::Throw) {
        throw std::runtime_error("injected read fault");
      }
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

 private:
  NtfsBrowserTests::MemoryDiskReader inner_;
  ULONGLONG fault_begin_;
  ULONGLONG fault_end_;
  Fault fault_;
  bool armed_ = false;
};

#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
// Fake images put their compressed or stored unit data at LCN 30 onwards.
constexpr ULONGLONG compressed_fault_begin =
    30ULL * NtfsBrowserTests::fake_cluster_size;
constexpr ULONGLONG compressed_fault_end =
    40ULL * NtfsBrowserTests::fake_cluster_size;
#endif

}  // namespace

namespace {

// Reads 16 bytes at an unaligned offset of the forged block, which fails in the
// given way on its first cluster.
template <Cache::Strategy S>
void CheckUnalignedFirstClusterFails(FaultyDiskReader::Fault fault) {
  const ULONGLONG block_begin =
      index_block_lcn * NtfsBrowserTests::fake_cluster_size;
  const ULONGLONG block_end = block_begin + NtfsBrowserTests::fake_cluster_size;
  auto reader = std::make_unique<FaultyDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock(), block_begin,
      block_end, fault);
  FaultyDiskReader* const faulty = reader.get();
  auto parsed = OpenRecordFrom<S>(std::move(reader),
                                  NtfsBrowserTests::index_alloc_dir_idx);
  faulty->Arm();
  REQUIRE(parsed.record->ParseAttrs());
  const auto& alloc = parsed.record->GetAttr(Attr::Type::IndexAllocation);
  REQUIRE(alloc.size() == 1);

  std::vector<BYTE> buffer(16);
  CHECK_FALSE(alloc[0]->ReadData(100, buffer).has_value());
}

}  // namespace

TEST_CASE("A cluster read that fails or throws yields no data, not a crash",
          "[cov-nri]") {
  // A FullCache volume serves these clusters from its cache, so only NoCache
  // reaches the failing read.
  SECTION("the unaligned first cluster read fails") {
    CheckUnalignedFirstClusterFails<Cache::Strategy::NoCache>(
        FaultyDiskReader::Fault::Fail);
  }

  SECTION("the unaligned first cluster read throws") {
    CheckUnalignedFirstClusterFails<Cache::Strategy::NoCache>(
        FaultyDiskReader::Fault::Throw);
  }
}

// Without decompression compiled in, ParseAttrs() rejects the compressed
// $DATA stream before any unit is read.
#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
TEST_CASE("A compression unit whose clusters cannot be read yields no data",
          "[cov-nri]") {
  // A FullCache volume serves these clusters from its cache, so only NoCache
  // reaches the failing read.
  // Root directory record: it carries the compressed $DATA stream.
  constexpr ULONGLONG root_record_idx = 5;

  const auto check_unit_read_fails = [](std::vector<BYTE> image) {
    auto reader = std::make_unique<FaultyDiskReader>(
        std::move(image), compressed_fault_begin, compressed_fault_end,
        FaultyDiskReader::Fault::Fail);
    FaultyDiskReader* const faulty = reader.get();
    auto parsed = OpenRecordFrom<Cache::Strategy::NoCache>(std::move(reader),
                                                           root_record_idx);
    faulty->Arm();
    REQUIRE(parsed.record->ParseAttrs());
    const auto& data = parsed.record->GetAttr(Attr::Type::Data);
    REQUIRE(data.size() == 1);

    std::vector<BYTE> buffer(NtfsBrowserTests::compression_unit_size);
    CHECK_FALSE(data[0]->ReadData(0, buffer).has_value());
  };

  SECTION("a compressed unit whose real clusters cannot be read") {
    check_unit_read_fails(
        NtfsBrowserTests::BuildFakeNtfsImageWithCompressedFile());
  }

  SECTION("a stored unit whose clusters cannot be read") {
    check_unit_read_fails(
        NtfsBrowserTests::BuildFakeNtfsImageWithStoredCompressionUnit());
  }
}
#endif

namespace {

// Every fake image puts $MFT at LCN 1.
constexpr ULONGLONG fake_mft_begin = NtfsBrowserTests::fake_cluster_size;

// The bytes of the records below Mft::Idx::User, read by the direct path.
constexpr ULONGLONG fake_mft_direct_end =
    fake_mft_begin + static_cast<ULONGLONG>(NtfsBrowser::Mft::Idx::User) *
                         NtfsBrowserTests::fake_file_record_size;

// Clusters of the data run that maps $MFT's fragmented records (see
// BuildFakeNtfsImageWithFragmentedMftInvalidRecord()).
constexpr ULONGLONG fragmented_mft_fault_begin =
    static_cast<ULONGLONG>(NtfsBrowserTests::fragmented_mft_data_run_lcn) *
    NtfsBrowserTests::fake_cluster_size;
constexpr ULONGLONG fragmented_mft_fault_end =
    static_cast<ULONGLONG>(NtfsBrowserTests::fragmented_mft_data_run_lcn +
                           NtfsBrowserTests::fragmented_mft_invalid_record_idx +
                           1) *
    NtfsBrowserTests::fake_cluster_size;

// A record number far past any $MFT extent.
constexpr ULONGLONG unmapped_record_idx = 1ULL << 40U;

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A record below Mft::Idx::User yields no record when its direct disk read "
    "fails",
    "[file-record][disk-read]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<FaultyDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), fake_mft_begin,
      fake_mft_direct_end, FaultyDiskReader::Fault::Fail);
  FaultyDiskReader* const faulty = reader.get();
  const NtfsBrowser::NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  faulty->Arm();

  NtfsBrowser::Io::FileRecord<S> record(volume);
  (void)NtfsBrowserTests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      static_cast<ULONGLONG>(NtfsBrowser::Mft::Idx::Root)));
  CHECK_FALSE(parsed);
  CHECK(NtfsBrowserTests::TakeCapturedLog().find("Cannot read file record 5") !=
        std::string::npos);
}

TEMPLATE_TEST_CASE_SIG(
    "A record past Mft::Idx::User yields no record when the $MFT data read "
    "behind it fails",
    "[file-record][disk-read]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<FaultyDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFragmentedMftInvalidRecord(),
      fragmented_mft_fault_begin, fragmented_mft_fault_end,
      FaultyDiskReader::Fault::Fail);
  FaultyDiskReader* const faulty = reader.get();
  const NtfsBrowser::NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  faulty->Arm();

  NtfsBrowser::Io::FileRecord<S> record(volume);
  (void)NtfsBrowserTests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      NtfsBrowserTests::fragmented_mft_invalid_record_idx));
  CHECK_FALSE(parsed);
  CHECK(NtfsBrowserTests::TakeCapturedLog().find(
            "Cannot read file record 16") != std::string::npos);
}

TEMPLATE_TEST_CASE_SIG("A record past every $MFT extent yields no record",
                       "[file-record][disk-read]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsBrowser::NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  (void)NtfsBrowserTests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(unmapped_record_idx));
  CHECK_FALSE(parsed);
  CHECK(NtfsBrowserTests::TakeCapturedLog().find(
            "Cannot read file record 1099511627776") != std::string::npos);
}

TEMPLATE_TEST_CASE_SIG(
    "A record below Mft::Idx::User whose header fails to parse yields no "
    "record",
    "[file-record][disk-read]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsBrowser::NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithForgedDirectRecord()));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  (void)NtfsBrowserTests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      NtfsBrowserTests::forged_direct_record_idx));
  CHECK_FALSE(parsed);
  const std::string log = NtfsBrowserTests::TakeCapturedLog();
  CHECK(log.find("Offset must be lower than 1024.") != std::string::npos);
  CHECK(log.find("Cannot read file record 12") != std::string::npos);
}

namespace {

// Byte offset of the first sector's last word, where a record keeps its update
// sequence number on disk.
constexpr size_t first_sector_fixup_offset = 510;

// Replaces the first sector's fixup word with one that matches neither the
// update sequence number nor the original word.
constexpr WORD unverified_fixup_word = 0xBEEF;

// A $MFT slot below Mft::Idx::User that BuildFakeNtfsImage() leaves free.
constexpr ULONGLONG free_direct_record_idx = 6;

// 2^53 records of 1 KiB put the byte offset at 2^63, past a LONGLONG.
constexpr ULONGLONG overflowing_direct_record_idx = 1ULL << 53U;

void PutWord(std::vector<BYTE>& image, size_t offset, WORD value) {
  std::memcpy(&image.at(offset), &value, sizeof(value));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A direct record whose byte offset overflows a LONGLONG yields no record "
    "and logs the narrowing error",
    "[file-record][disk-read][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // A volume whose $MFT does not parse has no extent map, so every record
  // takes the direct path.
  const NtfsBrowser::NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithCorruptMftRecord()));
  REQUIRE_FALSE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  (void)NtfsBrowserTests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed =
                      record.ParseFileRecord(overflowing_direct_record_idx));
  CHECK_FALSE(parsed);
  const std::string log = NtfsBrowserTests::TakeCapturedLog();
  CHECK_THAT(log, ContainsSubstring("past the end of $MFT"));
  CHECK_THAT(log,
             ContainsSubstring("Cannot read file record 9007199254740992"));
}

TEMPLATE_TEST_CASE_SIG(
    "A direct record without the file record signature is rejected as invalid, "
    "without a warning",
    "[file-record][disk-read][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsBrowser::NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  (void)NtfsBrowserTests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(free_direct_record_idx));
  CHECK_FALSE(parsed);
  const std::string log = NtfsBrowserTests::TakeCapturedLog();
  CHECK_THAT(log, ContainsSubstring("Invalid file record"));
  CHECK_THAT(log, !ContainsSubstring("Cannot read file record"));
  CHECK_THAT(log, !ContainsSubstring("Update Sequence Number error"));
}

TEMPLATE_TEST_CASE_SIG(
    "A direct record whose first sector fixup does not verify yields no record "
    "and warns",
    "[file-record][disk-read][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  PutWord(image,
          fake_mft_begin +
              static_cast<size_t>(NtfsBrowser::Mft::Idx::Root) *
                  NtfsBrowserTests::fake_file_record_size +
              first_sector_fixup_offset,
          unverified_fixup_word);
  const NtfsBrowser::NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  (void)NtfsBrowserTests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      static_cast<ULONGLONG>(NtfsBrowser::Mft::Idx::Root)));
  CHECK_FALSE(parsed);
  const std::string log = NtfsBrowserTests::TakeCapturedLog();
  CHECK_THAT(log, ContainsSubstring("Update Sequence Number error"));
  CHECK_THAT(log, !ContainsSubstring("Cannot read file record"));
}

TEMPLATE_TEST_CASE_SIG(
    "A direct record read that throws propagates the exception out of "
    "ParseFileRecord()",
    "[file-record][disk-read][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<FaultyDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), fake_mft_begin,
      fake_mft_direct_end, FaultyDiskReader::Fault::Throw);
  FaultyDiskReader* const faulty = reader.get();
  const NtfsBrowser::NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  faulty->Arm();

  NtfsBrowser::Io::FileRecord<S> record(volume);
  CHECK_THROWS_AS(static_cast<void>(record.ParseFileRecord(
                      static_cast<ULONGLONG>(NtfsBrowser::Mft::Idx::Root))),
                  std::runtime_error);
}
