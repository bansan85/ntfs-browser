#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "external-mft-support.h"
#include "standalone-mft-disk-reader.h"
#include "volume/volume-geometry.h"

namespace {

using NTFS_BROWSER_NAMESPACE::Tests::StandaloneMftDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::TempFile;

// Writes value at offset of bytes, little endian.
template <typename T>
void Put(std::vector<BYTE>& bytes, std::size_t offset, T value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(value));
}

// Offset of the update sequence array's own offset in a file record header.
constexpr std::size_t header_offset_of_us = 0x04;

// Offset of its size, in words.
constexpr std::size_t header_size_of_us = 0x06;

// Offset of the offset of the first attribute.
constexpr std::size_t header_offset_of_attr = 0x14;

// Offset of the record's allocated size, which is its size on the volume.
constexpr std::size_t header_alloc_size = 0x1C;

// Where the synthetic records put their update sequence array: right after
// the 48-byte header.
constexpr std::size_t update_sequence_at = 0x30;

// Where their first attribute begins: after the 3 words of the array of a
// 1024-byte record, rounded up to 8 bytes.
constexpr std::size_t first_attr_at = 0x38;

// A file record header of record_size bytes, with its update sequence array
// and an end-of-attributes marker, and no attribute. alloc_size is what the
// header declares, whatever record_size is.
std::vector<BYTE> MakeEmptyRecord(std::size_t record_size,
                                  std::uint32_t alloc_size) {
  std::vector<BYTE> bytes(record_size);
  Put<std::uint32_t>(bytes, 0, 0x454C4946);
  Put<std::uint16_t>(bytes, header_offset_of_us, update_sequence_at);
  Put<std::uint16_t>(bytes, header_size_of_us,
                     gsl::narrow<std::uint16_t>(record_size / 512 + 1));
  Put<std::uint16_t>(bytes, header_offset_of_attr, first_attr_at);
  Put<std::uint32_t>(bytes, header_alloc_size, alloc_size);
  // The update sequence number is 1, and each block end repeats it.
  Put<std::uint16_t>(bytes, update_sequence_at, 1);
  for (std::size_t block = 1; block <= record_size / 512; block++) {
    Put<std::uint16_t>(bytes, block * 512 - 2, 1);
  }
  Put<std::uint32_t>(bytes, first_attr_at, 0xFFFFFFFF);
  return bytes;
}

// Cluster size of the $MFT the synthetic record 0 describes: a $MFT of one
// 1024-byte record, in two 512-byte clusters.
constexpr DWORD synthetic_cluster_size = 512;

// Cluster where it begins: 16 KiB, beyond the 4 KiB boot area.
constexpr ULONGLONG synthetic_lcn = 32;

// A 1024-byte record 0 whose only attribute is a non-resident unnamed $DATA of
// two 512-byte clusters at synthetic_lcn.
std::vector<BYTE> MakeSyntheticRecord0() {
  std::vector<BYTE> bytes = MakeEmptyRecord(1024, 1024);
  constexpr std::size_t attr_size = 0x48;
  Put<std::uint32_t>(bytes, first_attr_at, 0x80);
  Put<std::uint32_t>(bytes, first_attr_at + 4, attr_size);
  // Non-resident, unnamed, run list at 0x40.
  Put<std::uint8_t>(bytes, first_attr_at + 8, 1);
  Put<std::uint64_t>(bytes, first_attr_at + 0x10, 0);
  Put<std::uint64_t>(bytes, first_attr_at + 0x18, 1);
  Put<std::uint16_t>(bytes, first_attr_at + 0x20, 0x40);
  Put<std::uint64_t>(bytes, first_attr_at + 0x28, 1024);
  Put<std::uint64_t>(bytes, first_attr_at + 0x30, 1024);
  Put<std::uint64_t>(bytes, first_attr_at + 0x38, 1024);
  // One run: a header byte with a 1-byte length and a 1-byte offset, 2
  // clusters, cluster synthetic_lcn, then the terminator.
  Put<std::uint8_t>(bytes, first_attr_at + 0x40, 0x11);
  Put<std::uint8_t>(bytes, first_attr_at + 0x41, 2);
  Put<std::uint8_t>(bytes, first_attr_at + 0x42,
                    gsl::narrow<std::uint8_t>(synthetic_lcn));
  Put<std::uint32_t>(bytes, first_attr_at + attr_size, 0xFFFFFFFF);
  return bytes;
}

}  // namespace

TEST_CASE("A synthetic record 0 opens as a standalone $MFT",
          "[standalone-mft][synthetic]") {
  const std::vector<BYTE> record = MakeSyntheticRecord0();
  const TempFile file(record);

  StandaloneMftDiskReader reader;
  REQUIRE(reader.Open(file.Path().wstring()));
  CHECK(reader.Failure().empty());
  CHECK(reader.FileRecordSize() == 1024);
  CHECK(reader.ClusterSize() == synthetic_cluster_size);
  CHECK(reader.FileSize() == 1024);
  CHECK(reader.Size() == (synthetic_lcn + 2) * synthetic_cluster_size);

  SECTION("byte 0 is a boot sector") {
    std::vector<BYTE> boot(512);
    LARGE_INTEGER addr{};
    REQUIRE(reader.ReadInto(addr, boot));
    const auto parse =
        NTFS_BROWSER_NAMESPACE::Volume::ParseBootSector(boot, false);
    REQUIRE(parse.ok);
    CHECK(parse.geometry.cluster_size == synthetic_cluster_size);
    CHECK(parse.geometry.file_record_size == 1024);
    CHECK(parse.geometry.mft_addr == synthetic_lcn * synthetic_cluster_size);
  }

  SECTION("the clusters of $DATA are the bytes of the file") {
    std::vector<BYTE> read(1024);
    LARGE_INTEGER addr{.QuadPart = gsl::narrow<LONGLONG>(
                           synthetic_lcn * synthetic_cluster_size)};
    REQUIRE(reader.ReadInto(addr, read));
    CHECK(read == record);

    // A read may start inside a run and end in it.
    std::vector<BYTE> middle(100);
    addr.QuadPart += 700;
    REQUIRE(reader.ReadInto(addr, middle));
    CHECK(std::equal(middle.begin(), middle.end(), record.begin() + 700));
  }

  SECTION("any other byte fails to read") {
    std::vector<BYTE> read(16);
    // Past the end of the last run.
    LARGE_INTEGER addr{.QuadPart = gsl::narrow<LONGLONG>(
                           (synthetic_lcn + 2) * synthetic_cluster_size)};
    CHECK_FALSE(reader.ReadInto(addr, read));
    // A read that starts in the run and ends outside it.
    addr.QuadPart -= 8;
    CHECK_FALSE(reader.ReadInto(addr, read));
    // Between the boot area and the run.
    addr.QuadPart = 8192;
    CHECK_FALSE(reader.ReadInto(addr, read));
    // Inside the boot area, past the boot sector: zeros.
    addr.QuadPart = 1024;
    CHECK(reader.ReadInto(addr, read));
    CHECK(std::ranges::all_of(read, [](BYTE byte) { return byte == 0; }));
    addr.QuadPart = -1;
    CHECK_FALSE(reader.ReadInto(addr, read));
  }
}

TEST_CASE("A file that is not a usable first $MFT record is refused",
          "[standalone-mft][synthetic]") {
  struct Case {
    std::string_view label;
    std::vector<BYTE> bytes;
  };

  std::vector<BYTE> bad_sequence = MakeSyntheticRecord0();
  Put<std::uint16_t>(bad_sequence, 510, 0x7777);
  std::vector<BYTE> short_array = MakeSyntheticRecord0();
  Put<std::uint16_t>(short_array, header_size_of_us, 2);
  std::vector<BYTE> array_out_of_record = MakeSyntheticRecord0();
  Put<std::uint16_t>(array_out_of_record, header_offset_of_us, 0xFFF0);
  std::vector<BYTE> sparse_first_run = MakeSyntheticRecord0();
  // A header byte with no offset field makes the run sparse.
  Put<std::uint8_t>(sparse_first_run, first_attr_at + 0x40, 0x01);
  std::vector<BYTE> cluster_not_a_power = MakeSyntheticRecord0();
  Put<std::uint64_t>(cluster_not_a_power, first_attr_at + 0x28, 1536);
  std::vector<BYTE> magic_baad = MakeSyntheticRecord0();
  Put<std::uint32_t>(magic_baad, 0, 0x44414142);
  const std::vector<Case> cases = {
      {"an empty file", {}},
      {"fewer bytes than a header", std::vector<BYTE>(47)},
      {"a zero-filled file", std::vector<BYTE>(4096)},
      {"a BAAD record 0", magic_baad},
      {"a record size of 0", MakeEmptyRecord(1024, 0)},
      {"a record size of 256", MakeEmptyRecord(1024, 256)},
      {"a record size of 1000", MakeEmptyRecord(1024, 1000)},
      {"a record size of 1 MiB", MakeEmptyRecord(1024, 1U << 20U)},
      {"a record size past the file", MakeEmptyRecord(1024, 4096)},
      {"a record 0 without $DATA", MakeEmptyRecord(1024, 1024)},
      {"a failed update sequence", bad_sequence},
      {"an update sequence array too short", short_array},
      {"an update sequence array outside the record", array_out_of_record},
      {"a sparse first run", sparse_first_run},
      {"a cluster size that is not a power of two", cluster_not_a_power},
  };
  for (const Case& test : cases) {
    CAPTURE(test.label);
    const TempFile file(test.bytes);
    StandaloneMftDiskReader reader;
    CHECK_FALSE(reader.Open(file.Path().wstring()));
    CHECK_FALSE(reader.Failure().empty());
    CHECK_FALSE(reader.Size().has_value());
    LARGE_INTEGER addr{};
    std::vector<BYTE> read(16);
    CHECK_FALSE(reader.ReadInto(addr, read));
  }
}

TEST_CASE("A missing file is refused as a standalone $MFT",
          "[standalone-mft][synthetic]") {
  StandaloneMftDiskReader reader;
  CHECK_FALSE(reader.Open(
      (std::filesystem::temp_directory_path() / "no-such-mft-file").wstring()));
  CHECK_FALSE(reader.Failure().empty());
}
