#include <ntfs-browser/win-types.h>

#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "nonresident-index-support.h"

namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;

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
