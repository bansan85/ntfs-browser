#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far smaller than Data::IndexBlock",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<
      NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithTinyIndexBlock());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK(volume.GetIndexBlockSize() ==
        NTFS_BROWSER_NAMESPACE::Tests::tiny_index_block_size);

  // tiny_index_block_size doesn't fit Data::IndexBlock's own header.
  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far larger than any plausible size",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithOversizedIndexBlock());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "far larger than any plausible size",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithOversizedFileRecord());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetFileRecordSize() = " << volume.GetFileRecordSize());
  // GetFileRecordSize() reflects the BPB value directly; IsVolumeOK() fails
  // regardless, from an unrelated $Volume read failure this size triggers.
  CHECK(volume.GetFileRecordSize() !=
        NTFS_BROWSER_NAMESPACE::Tests::oversized_file_record_size);
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "size that exceeds max_file_record_size via the positive "
    "clusters_per_file_record branch",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithFileRecordSizeTooBig());

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.GetFileRecordSize() ==
        NTFS_BROWSER_NAMESPACE::Tests::file_record_size_too_big);
  CHECK_FALSE(volume.IsVolumeOK());
}

namespace {

// Sector size of the emulated 4Kn disk: every read at offset 0 MUST be a
// multiple of it.
constexpr size_t sector4_kn = 4096;

// Wraps an in-memory image and rejects a read at offset 0 whose length the
// emulated medium would refuse. The boot sector is the only such read.
class StrictBootReadReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // reject decides, from the read length, whether the read at 0 fails.
  StrictBootReadReader(std::vector<BYTE> image, bool (*reject)(size_t length))
      : inner_(std::move(image)), reject_(reject) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    if (addr.QuadPart == 0 && reject_(dest.size())) {
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

 private:
  NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader inner_;
  bool (*reject_)(size_t length);
};

bool NotMultipleOf4Kn(size_t length) { return length % sector4_kn != 0; }

bool AtLeast4Kn(size_t length) { return length >= sector4_kn; }

bool Always(size_t /*length*/) { return true; }

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume reads the boot sector in a whole 4Kn sector, so an unbuffered "
    "device accepts it",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<StrictBootReadReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage(), &NotMultipleOf4Kn);

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume still opens a medium too short to serve a whole 4Kn sector",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<StrictBootReadReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage(), &AtLeast4Kn);

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a volume whose boot sector cannot be read at all",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<StrictBootReadReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage(), &Always);

  const NtfsVolume<S> volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());
}

namespace {

// BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
std::vector<BYTE> ImageWithBpb(Edit edit) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  edit(*reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data()));
  return image;
}

// Whether a volume came up, and the log its opening wrote.
struct Opened {
  bool ok{false};
  std::string log;
};

// Opens reader, and returns whether the volume came up with the log the open
// wrote.
template <Cache::Strategy S>
Opened OpenLogged(std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader) {
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NtfsVolume<S> volume(std::move(reader));
  return {.ok = volume.IsVolumeOK(),
          .log = NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog()};
}

// Opens the fake image with edit applied to its boot sector BPB.
template <Cache::Strategy S, typename Edit>
Opened OpenWithBpb(Edit edit) {
  return OpenLogged<S>(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          ImageWithBpb(edit)));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume logs both boot sector read attempts when the medium cannot "
    "serve either one",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened = OpenLogged<S>(std::make_unique<StrictBootReadReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage(), &Always));

  CHECK_FALSE(opened.ok);
  CHECK_THAT(opened.log,
             ContainsSubstring("Cannot read a 4096-byte boot sector, "
                               "retrying with 512 bytes"));
  CHECK_THAT(opened.log, ContainsSubstring("Read boot sector error"));
  CHECK_THAT(opened.log, !ContainsSubstring("Sector Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume falls back to a 512-byte boot sector read and logs the "
    "fallback",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened = OpenLogged<S>(std::make_unique<StrictBootReadReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage(), &AtLeast4Kn));

  CHECK(opened.ok);
  CHECK_THAT(opened.log,
             ContainsSubstring("Cannot read a 4096-byte boot sector, "
                               "retrying with 512 bytes"));
  CHECK_THAT(opened.log, ContainsSubstring("Sector Size = 512 bytes"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a boot sector without the NTFS signature",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened =
      OpenWithBpb<S>([](auto& bpb) { bpb.signature[0] = 'X'; });

  CHECK_FALSE(opened.ok);
  CHECK_THAT(opened.log, ContainsSubstring("Volume file system is not NTFS"));
  CHECK_THAT(opened.log, !ContainsSubstring("Sector Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a sectors_per_cluster magnitude beyond -12",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened = OpenWithBpb<S>(
      [](auto& bpb) { bpb.sectors_per_cluster = static_cast<BYTE>(-13); });

  CHECK_FALSE(opened.ok);
  CHECK_THAT(opened.log,
             ContainsSubstring("sectors_per_cluster magnitude out of range"));
  CHECK_THAT(opened.log, !ContainsSubstring("Cluster Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a zero sectors_per_cluster as a null cluster size",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened =
      OpenWithBpb<S>([](auto& bpb) { bpb.sectors_per_cluster = 0; });

  CHECK_FALSE(opened.ok);
  CHECK_THAT(opened.log, ContainsSubstring("Cluster Size = 0 bytes"));
  CHECK_THAT(opened.log, ContainsSubstring("Cluster Size can't be null"));
  CHECK_THAT(opened.log, !ContainsSubstring("FileRecord Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume accepts clusters_per_file_record of -12 and rejects -13 and 9",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("-12 gives a 4096-byte file record") {
    const Opened opened = OpenWithBpb<S>([](auto& bpb) {
      bpb.clusters_per_file_record = static_cast<DWORD>(-12);
    });

    CHECK_THAT(opened.log, ContainsSubstring("FileRecord Size = 4096 bytes"));
    CHECK_THAT(opened.log, !ContainsSubstring("magnitude out of range"));
  }

  SECTION("-13 is out of range") {
    const Opened opened = OpenWithBpb<S>([](auto& bpb) {
      bpb.clusters_per_file_record = static_cast<DWORD>(-13);
    });

    CHECK_FALSE(opened.ok);
    CHECK_THAT(
        opened.log,
        ContainsSubstring("clusters_per_file_record magnitude out of range"));
  }

  SECTION("9 clusters is out of range") {
    const Opened opened =
        OpenWithBpb<S>([](auto& bpb) { bpb.clusters_per_file_record = 9; });

    CHECK_FALSE(opened.ok);
    CHECK_THAT(
        opened.log,
        ContainsSubstring("clusters_per_file_record magnitude out of range"));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a file record that is not a whole number of sectors",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened = OpenWithBpb<S>(
      [](auto& bpb) { bpb.clusters_per_file_record = static_cast<DWORD>(-6); });

  CHECK_FALSE(opened.ok);
  CHECK_THAT(opened.log, ContainsSubstring("FileRecord Size = 64 bytes"));
  CHECK_THAT(opened.log, ContainsSubstring("FileRecord Size is invalid"));
  CHECK_THAT(opened.log, !ContainsSubstring("IndexBlock Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a file record larger than the maximum",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened =
      OpenWithBpb<S>([](auto& bpb) { bpb.clusters_per_file_record = 5; });

  CHECK_FALSE(opened.ok);
  CHECK_THAT(opened.log, ContainsSubstring("FileRecord Size = 5120 bytes"));
  CHECK_THAT(
      opened.log,
      ContainsSubstring("FileRecord Size exceeds the maximum supported file "
                        "record size"));
  CHECK_THAT(opened.log, !ContainsSubstring("IndexBlock Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects clusters_per_index_block magnitudes out of range",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const BYTE raw : {BYTE{0xF3}, BYTE{9}}) {
    INFO("clusters_per_index_block = " << static_cast<int>(raw));
    const Opened opened = OpenWithBpb<S>(
        [raw](auto& bpb) { bpb.clusters_per_index_block = raw; });

    CHECK_FALSE(opened.ok);
    CHECK_THAT(
        opened.log,
        ContainsSubstring("clusters_per_index_block magnitude out of range"));
    CHECK_THAT(opened.log, !ContainsSubstring("MFT address = "));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects an index block that is not a whole number of sectors",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened = OpenWithBpb<S>(
      [](auto& bpb) { bpb.clusters_per_index_block = static_cast<DWORD>(-7); });

  CHECK_FALSE(opened.ok);
  CHECK_THAT(opened.log, ContainsSubstring("IndexBlock Size = 128 bytes"));
  CHECK_THAT(opened.log, ContainsSubstring("IndexBlock Size is invalid"));
  CHECK_THAT(opened.log, !ContainsSubstring("MFT address = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume accepts a whole-sector index block of 512 bytes",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened = OpenWithBpb<S>(
      [](auto& bpb) { bpb.clusters_per_index_block = static_cast<DWORD>(-9); });

  CHECK_THAT(opened.log, ContainsSubstring("IndexBlock Size = 512 bytes"));
  CHECK_THAT(opened.log, !ContainsSubstring("IndexBlock Size is invalid"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects an MFT address whose byte offset overflows",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened opened = OpenWithBpb<S>(
      [](auto& bpb) { bpb.lcn_mft = std::numeric_limits<ULONGLONG>::max(); });

  CHECK_FALSE(opened.ok);
  CHECK_THAT(opened.log, ContainsSubstring("MFT address = 0xFFFFFFFFFFFFFFFF"));
  CHECK_THAT(opened.log, ContainsSubstring("MFT address is invalid"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects an MFT address past the plausible limit without "
    "overflowing",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // 2^52 clusters of 1024 bytes is 2^62 bytes: just above LONGLONG_MAX / 2,
  // and still within a ULONGLONG.
  const Opened opened =
      OpenWithBpb<S>([](auto& bpb) { bpb.lcn_mft = 1ULL << 52U; });

  CHECK_FALSE(opened.ok);
  CHECK_THAT(opened.log, ContainsSubstring("MFT address = 0x4000000000000000"));
  CHECK_THAT(opened.log, ContainsSubstring("MFT address is invalid"));
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume accepts the largest plausible MFT address",
                       "[ntfs-volume][regression]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // (2^52 - 1) clusters of 1024 bytes: the last offset under LONGLONG_MAX / 2.
  // The volume declares the most sectors it can, so the first $MFT record
  // fits in it.
  const Opened opened = OpenWithBpb<S>([](auto& bpb) {
    bpb.lcn_mft = (1ULL << 52U) - 1;
    bpb.total_sectors = std::numeric_limits<ULONGLONG>::max();
  });

  CHECK_THAT(opened.log, ContainsSubstring("MFT address = 0x3FFFFFFFFFFFFC00"));
  CHECK_THAT(opened.log, !ContainsSubstring("MFT address is invalid"));
  CHECK_THAT(opened.log, !ContainsSubstring("First $MFT record lies beyond"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume logs the boot sector geometry in validation order",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage()));
  const std::string log = NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

  CHECK(volume.GetSectorSize() ==
        NTFS_BROWSER_NAMESPACE::Tests::fake_bytes_per_sector);
  CHECK(volume.GetClusterSize() ==
        NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
  CHECK(volume.GetFileRecordSize() == 1024);
  CHECK(volume.GetIndexBlockSize() == 1024);

  const std::size_t sector = log.find("Sector Size = 512 bytes");
  const std::size_t cluster = log.find("Cluster Size = 1024 bytes");
  const std::size_t record = log.find("FileRecord Size = 1024 bytes");
  const std::size_t index = log.find("IndexBlock Size = 1024 bytes");
  const std::size_t mft = log.find("MFT address = 0x");
  REQUIRE(sector != std::string::npos);
  CHECK(sector < cluster);
  CHECK(cluster < record);
  CHECK(record < index);
  CHECK(index < mft);
}
