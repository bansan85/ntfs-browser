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
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/catch_message.hpp"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far smaller than Data::IndexBlock",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithTinyIndexBlock());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK(volume.GetIndexBlockSize() == NtfsBrowserTests::tiny_index_block_size);

  // tiny_index_block_size doesn't fit Data::IndexBlock's own header.
  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far larger than any plausible size",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedIndexBlock());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "far larger than any plausible size",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedFileRecord());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetFileRecordSize() = " << volume.GetFileRecordSize());
  // GetFileRecordSize() reflects the BPB value directly; IsVolumeOK() fails
  // regardless, from an unrelated $Volume read failure this size triggers.
  CHECK(volume.GetFileRecordSize() !=
        NtfsBrowserTests::oversized_file_record_size);
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "size that exceeds max_file_record_size via the positive "
    "clusters_per_file_record branch",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFileRecordSizeTooBig());

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.GetFileRecordSize() ==
        NtfsBrowserTests::file_record_size_too_big);
  CHECK_FALSE(volume.IsVolumeOK());
}

namespace {

// Sector size of the emulated 4Kn disk: every read at offset 0 MUST be a
// multiple of it.
constexpr size_t sector4_kn = 4096;

// Wraps an in-memory image and rejects a read at offset 0 whose length the
// emulated medium would refuse. The boot sector is the only such read.
class StrictBootReadReader : public NtfsBrowser::IDiskReader {
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
  NtfsBrowserTests::MemoryDiskReader inner_;
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
      NtfsBrowserTests::BuildFakeNtfsImage(), &NotMultipleOf4Kn);

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume still opens a medium too short to serve a whole 4Kn sector",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<StrictBootReadReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), &AtLeast4Kn);

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a volume whose boot sector cannot be read at all",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<StrictBootReadReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), &Always);

  const NtfsVolume<S> volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());
}

namespace {

// BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
std::vector<BYTE> ImageWithBpb(Edit edit) {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  edit(*reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data()));
  return image;
}

// Opens reader, and reports whether the volume came up. Returns the log the
// open wrote.
template <Cache::Strategy S>
std::string LogWhenOpened(std::unique_ptr<NtfsBrowser::IDiskReader> reader,
                          bool& volume_ok) {
  (void)NtfsBrowserTests::TakeCapturedLog();
  const NtfsVolume<S> volume(std::move(reader));
  volume_ok = volume.IsVolumeOK();
  return NtfsBrowserTests::TakeCapturedLog();
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume logs both boot sector read attempts when the medium cannot "
    "serve either one",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = true;
  const std::string log =
      LogWhenOpened<S>(std::make_unique<StrictBootReadReader>(
                           NtfsBrowserTests::BuildFakeNtfsImage(), &Always),
                       volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("Cannot read a 4096-byte boot sector, "
                                    "retrying with 512 bytes"));
  CHECK_THAT(log, ContainsSubstring("Read boot sector error"));
  CHECK_THAT(log, !ContainsSubstring("Sector Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume falls back to a 512-byte boot sector read and logs the "
    "fallback",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = false;
  const std::string log =
      LogWhenOpened<S>(std::make_unique<StrictBootReadReader>(
                           NtfsBrowserTests::BuildFakeNtfsImage(), &AtLeast4Kn),
                       volume_ok);

  CHECK(volume_ok);
  CHECK_THAT(log, ContainsSubstring("Cannot read a 4096-byte boot sector, "
                                    "retrying with 512 bytes"));
  CHECK_THAT(log, ContainsSubstring("Sector Size = 512 bytes"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a boot sector without the NTFS signature",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = true;
  const std::string log = LogWhenOpened<S>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          ImageWithBpb([](auto& bpb) { bpb.signature[0] = 'X'; })),
      volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("Volume file system is not NTFS"));
  CHECK_THAT(log, !ContainsSubstring("Sector Size = "));
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume rejects a sector size below two bytes",
                       "[ntfs-volume][regression]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const WORD sector_size : {WORD{0}, WORD{1}}) {
    INFO("bytes_per_sector = " << sector_size);
    bool volume_ok = true;
    const std::string log = LogWhenOpened<S>(
        std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWithBpb(
            [sector_size](auto& bpb) { bpb.bytes_per_sector = sector_size; })),
        volume_ok);

    CHECK_FALSE(volume_ok);
    CHECK_THAT(log, ContainsSubstring("Sector Size must be at least 2 bytes"));
    CHECK_THAT(log, !ContainsSubstring("Cluster Size = "));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a sectors_per_cluster magnitude beyond -12",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = true;
  const std::string log = LogWhenOpened<S>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWithBpb(
          [](auto& bpb) { bpb.sectors_per_cluster = static_cast<BYTE>(-13); })),
      volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log,
             ContainsSubstring("sectors_per_cluster magnitude out of range"));
  CHECK_THAT(log, !ContainsSubstring("Cluster Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a zero sectors_per_cluster as a null cluster size",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = true;
  const std::string log = LogWhenOpened<S>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          ImageWithBpb([](auto& bpb) { bpb.sectors_per_cluster = 0; })),
      volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("Cluster Size = 0 bytes"));
  CHECK_THAT(log, ContainsSubstring("Cluster Size can't be null"));
  CHECK_THAT(log, !ContainsSubstring("FileRecord Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume accepts clusters_per_file_record of -12 and rejects -13 and 9",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("-12 gives a 4096-byte file record") {
    bool volume_ok = false;
    const std::string log =
        LogWhenOpened<S>(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
                             ImageWithBpb([](auto& bpb) {
                               bpb.clusters_per_file_record =
                                   static_cast<DWORD>(-12);
                             })),
                         volume_ok);

    CHECK_THAT(log, ContainsSubstring("FileRecord Size = 4096 bytes"));
    CHECK_THAT(log, !ContainsSubstring("magnitude out of range"));
  }

  SECTION("-13 is out of range") {
    bool volume_ok = true;
    const std::string log =
        LogWhenOpened<S>(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
                             ImageWithBpb([](auto& bpb) {
                               bpb.clusters_per_file_record =
                                   static_cast<DWORD>(-13);
                             })),
                         volume_ok);

    CHECK_FALSE(volume_ok);
    CHECK_THAT(log, ContainsSubstring(
                        "clusters_per_file_record magnitude out of range"));
  }

  SECTION("9 clusters is out of range") {
    bool volume_ok = true;
    const std::string log = LogWhenOpened<S>(
        std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
            ImageWithBpb([](auto& bpb) { bpb.clusters_per_file_record = 9; })),
        volume_ok);

    CHECK_FALSE(volume_ok);
    CHECK_THAT(log, ContainsSubstring(
                        "clusters_per_file_record magnitude out of range"));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a file record smaller than its header",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // 2-byte sectors keep a 16-byte record a whole number of sectors, so only
  // the header-size check can reject it.
  bool volume_ok = true;
  const std::string log =
      LogWhenOpened<S>(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
                           ImageWithBpb([](auto& bpb) {
                             bpb.bytes_per_sector = 2;
                             bpb.sectors_per_cluster = 32;
                             bpb.clusters_per_file_record =
                                 static_cast<DWORD>(-4);
                           })),
                       volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("FileRecord Size = 16 bytes"));
  CHECK_THAT(log, ContainsSubstring("FileRecord Size is invalid"));
  CHECK_THAT(log, !ContainsSubstring("IndexBlock Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a file record that is not a whole number of sectors",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = true;
  const std::string log =
      LogWhenOpened<S>(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
                           ImageWithBpb([](auto& bpb) {
                             bpb.clusters_per_file_record =
                                 static_cast<DWORD>(-6);
                           })),
                       volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("FileRecord Size = 64 bytes"));
  CHECK_THAT(log, ContainsSubstring("FileRecord Size is invalid"));
  CHECK_THAT(log, !ContainsSubstring("IndexBlock Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a file record larger than the maximum",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = true;
  const std::string log = LogWhenOpened<S>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          ImageWithBpb([](auto& bpb) { bpb.clusters_per_file_record = 5; })),
      volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("FileRecord Size = 5120 bytes"));
  CHECK_THAT(log, ContainsSubstring(
                      "FileRecord Size exceeds the maximum supported file "
                      "record size"));
  CHECK_THAT(log, !ContainsSubstring("IndexBlock Size = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects clusters_per_index_block magnitudes out of range",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const BYTE raw : {BYTE{0xF3}, BYTE{9}}) {
    INFO("clusters_per_index_block = " << static_cast<int>(raw));
    bool volume_ok = true;
    const std::string log = LogWhenOpened<S>(
        std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWithBpb(
            [raw](auto& bpb) { bpb.clusters_per_index_block = raw; })),
        volume_ok);

    CHECK_FALSE(volume_ok);
    CHECK_THAT(log, ContainsSubstring(
                        "clusters_per_index_block magnitude out of range"));
    CHECK_THAT(log, !ContainsSubstring("MFT address = "));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects an index block smaller than its header",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // 2-byte sectors keep a 2-byte index block a whole number of sectors, so
  // only the header-size check can reject it.
  bool volume_ok = true;
  const std::string log =
      LogWhenOpened<S>(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
                           ImageWithBpb([](auto& bpb) {
                             bpb.bytes_per_sector = 2;
                             bpb.sectors_per_cluster = 32;
                             bpb.clusters_per_file_record = 1;
                             bpb.clusters_per_index_block =
                                 static_cast<DWORD>(-1);
                           })),
                       volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("IndexBlock Size = 2 bytes"));
  CHECK_THAT(log, ContainsSubstring("IndexBlock Size is invalid"));
  CHECK_THAT(log, !ContainsSubstring("MFT address = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects an index block that is not a whole number of sectors",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = true;
  const std::string log =
      LogWhenOpened<S>(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
                           ImageWithBpb([](auto& bpb) {
                             bpb.clusters_per_index_block =
                                 static_cast<DWORD>(-7);
                           })),
                       volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("IndexBlock Size = 128 bytes"));
  CHECK_THAT(log, ContainsSubstring("IndexBlock Size is invalid"));
  CHECK_THAT(log, !ContainsSubstring("MFT address = "));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume accepts a whole-sector index block of 512 bytes",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = false;
  const std::string log =
      LogWhenOpened<S>(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
                           ImageWithBpb([](auto& bpb) {
                             bpb.clusters_per_index_block =
                                 static_cast<DWORD>(-9);
                           })),
                       volume_ok);

  CHECK_THAT(log, ContainsSubstring("IndexBlock Size = 512 bytes"));
  CHECK_THAT(log, !ContainsSubstring("IndexBlock Size is invalid"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects an MFT address whose byte offset overflows",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  bool volume_ok = true;
  const std::string log =
      LogWhenOpened<S>(std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
                           ImageWithBpb([](auto& bpb) {
                             bpb.lcn_mft =
                                 std::numeric_limits<ULONGLONG>::max();
                           })),
                       volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("MFT address = 0xFFFFFFFFFFFFFFFF"));
  CHECK_THAT(log, ContainsSubstring("MFT address is invalid"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects an MFT address past the plausible limit without "
    "overflowing",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // 2^52 clusters of 1024 bytes is 2^62 bytes: just above LONGLONG_MAX / 2,
  // and still within a ULONGLONG.
  bool volume_ok = true;
  const std::string log = LogWhenOpened<S>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          ImageWithBpb([](auto& bpb) { bpb.lcn_mft = 1ULL << 52U; })),
      volume_ok);

  CHECK_FALSE(volume_ok);
  CHECK_THAT(log, ContainsSubstring("MFT address = 0x4000000000000000"));
  CHECK_THAT(log, ContainsSubstring("MFT address is invalid"));
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume accepts the largest plausible MFT address",
                       "[ntfs-volume][regression]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // (2^52 - 1) clusters of 1024 bytes: the last offset under LONGLONG_MAX / 2.
  bool volume_ok = true;
  const std::string log = LogWhenOpened<S>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          ImageWithBpb([](auto& bpb) { bpb.lcn_mft = (1ULL << 52U) - 1; })),
      volume_ok);

  CHECK_THAT(log, ContainsSubstring("MFT address = 0x3FFFFFFFFFFFFC00"));
  CHECK_THAT(log, !ContainsSubstring("MFT address is invalid"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume logs the boot sector geometry in validation order",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  (void)NtfsBrowserTests::TakeCapturedLog();
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImage()));
  const std::string log = NtfsBrowserTests::TakeCapturedLog();

  CHECK(volume.GetSectorSize() == NtfsBrowserTests::fake_bytes_per_sector);
  CHECK(volume.GetClusterSize() == NtfsBrowserTests::fake_cluster_size);
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
