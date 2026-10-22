#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
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
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "catch2/catch_tostring.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// Logged when the backup boot sector replaces the unusable one.
constexpr const char* backup_message = "using the backup boot sector";

// Logged when the size of the volume is not known, or too small.
constexpr const char* no_size_message = "the size of the volume is unknown";

// Logged when no candidate sector size gives a usable backup.
constexpr const char* no_candidate_message = "No usable backup boot sector";

// Bytes of a boot sector: the shortest read of a candidate.
constexpr size_t boot_sector_bytes = 512;

// Sector sizes the backup may declare: the candidates the reader tries.
constexpr std::array<WORD, 4> sector_sizes{512, 1024, 2048, 4096};

// Extra sectors that make an image's size no multiple of a cache block.
constexpr size_t extra_sectors = 3;

// An image smaller than one 64 KiB cache block, big enough for the records.
constexpr size_t small_image_bytes = size_t{16} * 1024;

// A MemoryDiskReader that remembers where each read started, and how often it
// was asked for its size. It reports the size it is given.
class SpyDiskReader final : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  struct Record {
    std::vector<LONGLONG> starts;
    size_t size_calls{0};
  };

  SpyDiskReader(std::vector<BYTE> image, std::optional<ULONGLONG> size,
                std::shared_ptr<Record> record)
      : inner_(std::move(image)), size_(size), record_(std::move(record)) {}

  bool Open(std::wstring_view /*path*/) override { return false; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    record_->starts.push_back(addr.QuadPart);
    return inner_.ReadInto(addr, dest);
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    record_->size_calls++;
    return size_;
  }

 private:
  Tests::MemoryDiskReader inner_;
  std::optional<ULONGLONG> size_;
  std::shared_ptr<Record> record_;
};

// An opened volume, with what the open read and logged.
template <Cache::Strategy S>
struct Opened {
  std::unique_ptr<NtfsVolume<S>> volume;
  std::shared_ptr<SpyDiskReader::Record> record;
  std::string log;
  size_t image_bytes{0};

  // Reads that started within the last boot sector's worth of bytes of the
  // image: where a backup boot sector sits.
  [[nodiscard]] size_t BackupReads() const {
    size_t count = 0;
    for (const LONGLONG start : record->starts) {
      count += static_cast<size_t>(start) + boot_sector_bytes >= image_bytes;
    }
    return count;
  }
};

// Opens the image over a reader that reports reported_size as its size.
template <Cache::Strategy S>
Opened<S> Open(std::vector<BYTE> image, const VolumeOptions& options,
               std::optional<ULONGLONG> reported_size) {
  (void)Tests::TakeCapturedLog();
  const size_t image_bytes = image.size();
  auto record = std::make_shared<SpyDiskReader::Record>();
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<SpyDiskReader>(std::move(image), reported_size, record),
      options);
  return {.volume = std::move(volume),
          .record = std::move(record),
          .log = Tests::TakeCapturedLog(),
          .image_bytes = image_bytes};
}

// Opens the image over a reader that reports its real size.
template <Cache::Strategy S>
Opened<S> Open(std::vector<BYTE> image, bool recover) {
  const ULONGLONG size = image.size();
  return Open<S>(std::move(image), VolumeOptions{.recover_errors = recover},
                 size);
}

// A volume whose boot sector at offset 0 is damaged, with a sound copy.
std::vector<BYTE>
    Image(Tests::BootSectorDamage sector0 = Tests::BootSectorDamage::Zeroed,
          WORD sector_size = Tests::fake_bytes_per_sector) {
  return Tests::BuildFakeNtfsImageWithBackupBootSector(
      {.sector_size = sector_size, .sector0 = sector0});
}

// Every way a boot sector can be broken.
constexpr std::array<Tests::BootSectorDamage, 3> damages{
    Tests::BootSectorDamage::Zeroed, Tests::BootSectorDamage::Signature,
    Tests::BootSectorDamage::SectorSize};

}  // namespace

TEMPLATE_TEST_CASE_SIG("a strict volume never reads the backup boot sector",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const auto damage : damages) {
    INFO("damage = " << static_cast<int>(damage));
    const Opened<S> opened = Open<S>(Image(damage), false);

    CHECK_FALSE(opened.volume->IsVolumeOK());
    CHECK(opened.BackupReads() == 0);
    CHECK(opened.record->size_calls == 0);
    CHECK_THAT(opened.log, !ContainsSubstring(backup_message));
  }
}

TEMPLATE_TEST_CASE_SIG("a sound boot sector never looks for the backup",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const bool recover : {false, true}) {
    INFO("recover_errors = " << recover);
    const Opened<S> opened =
        Open<S>(Image(Tests::BootSectorDamage::None), recover);

    CHECK(opened.volume->IsVolumeOK());
    CHECK(opened.BackupReads() == 0);
    CHECK(opened.record->size_calls == 0);
    CHECK_THAT(opened.log, !ContainsSubstring(backup_message));
  }
}

TEMPLATE_TEST_CASE_SIG("a recovering volume uses the backup boot sector",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const auto damage : damages) {
    INFO("damage = " << static_cast<int>(damage));
    const Opened<S> opened = Open<S>(Image(damage), true);

    CHECK(opened.volume->IsVolumeOK());
    CHECK(opened.BackupReads() >= 1);
    CHECK(opened.volume->GetSectorSize() == Tests::fake_bytes_per_sector);
    CHECK(opened.volume->GetClusterSize() == Tests::fake_cluster_size);
    CHECK(opened.volume->GetFileRecordSize() == Tests::fake_file_record_size);
    CHECK_THAT(opened.log,
               ContainsSubstring(std::format("{} at 0x{:X}", backup_message,
                                             opened.image_bytes - 512)));
  }
}

TEMPLATE_TEST_CASE_SIG("the backup boot sector gives the same volume",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened<S> healthy = Open<S>(Image(Tests::BootSectorDamage::None), true);
  const Opened<S> recovered = Open<S>(Image(), true);
  REQUIRE(healthy.volume->IsVolumeOK());
  REQUIRE(recovered.volume->IsVolumeOK());

  CHECK(recovered.volume->GetMFTAddr() == healthy.volume->GetMFTAddr());
  CHECK(recovered.volume->GetIndexBlockSize() ==
        healthy.volume->GetIndexBlockSize());
  CHECK(recovered.volume->GetRecordsCount() ==
        healthy.volume->GetRecordsCount());
  CHECK(recovered.volume->GetClusterBuffer().size() ==
        healthy.volume->GetClusterBuffer().size());
}

TEMPLATE_TEST_CASE_SIG("the backup boot sector may use any sector size",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const WORD sector_size : sector_sizes) {
    INFO("sector size = " << sector_size);
    const Opened<S> opened =
        Open<S>(Image(Tests::BootSectorDamage::Zeroed, sector_size), true);

    // Past 1024 bytes a record is a cluster, which the 1024-byte fake records
    // do not fill: the boot sector is adopted, but the volume does not open.
    CHECK(opened.volume->IsVolumeOK() == (sector_size <= 1024));
    CHECK(opened.volume->GetSectorSize() == sector_size);
    CHECK(opened.volume->GetFileRecordSize() ==
          std::max<DWORD>(Tests::fake_file_record_size, sector_size));
    const size_t read_bytes = std::max<size_t>(sector_size, boot_sector_bytes);
    CHECK_THAT(opened.log,
               ContainsSubstring(std::format("{} at 0x{:X}", backup_message,
                                             opened.image_bytes - read_bytes)));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "the backup boot sector works near the end of the medium",
    "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const size_t image_bytes :
       {small_image_bytes,
        Tests::fake_backup_image_bytes + extra_sectors * boot_sector_bytes}) {
    INFO("image bytes = " << image_bytes);
    const Opened<S> opened =
        Open<S>(Tests::BuildFakeNtfsImageWithBackupBootSector(
                    {.image_bytes = image_bytes}),
                true);

    CHECK(opened.volume->IsVolumeOK());
    CHECK_THAT(opened.log, ContainsSubstring(backup_message));
  }
}

TEMPLATE_TEST_CASE_SIG("the backup boot sector works through read-ahead",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  std::vector<BYTE> image = Image();
  const ULONGLONG size = image.size();
  const Opened<S> opened = Open<S>(
      std::move(image),
      VolumeOptions{.recover_errors = true, .read_ahead_bytes = 4096}, size);

  CHECK(opened.volume->IsVolumeOK());
  CHECK_THAT(opened.log, ContainsSubstring(backup_message));
}

TEMPLATE_TEST_CASE_SIG("a damaged backup boot sector rejects the volume",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const auto damage : damages) {
    INFO("damage = " << static_cast<int>(damage));
    const Opened<S> opened = Open<S>(
        Tests::BuildFakeNtfsImageWithBackupBootSector(
            {.sector0 = Tests::BootSectorDamage::Zeroed, .backup = damage}),
        true);

    CHECK_FALSE(opened.volume->IsVolumeOK());
    CHECK_THAT(opened.log, ContainsSubstring(no_candidate_message));
    CHECK_THAT(opened.log, !ContainsSubstring(backup_message));
  }
}

TEMPLATE_TEST_CASE_SIG("a rejected volume keeps the geometry of sector 0",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // Sector 0 declares a size that is no power of two, which is decoded before
  // the parse rejects it. The backup is lost as well.
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithBackupBootSector(
      {.sector0 = Tests::BootSectorDamage::SectorSize,
       .backup = Tests::BootSectorDamage::Zeroed});
  const Opened<S> strict = Open<S>(image, false);
  const Opened<S> recovering = Open<S>(std::move(image), true);

  CHECK_FALSE(recovering.volume->IsVolumeOK());
  CHECK(recovering.volume->GetSectorSize() == strict.volume->GetSectorSize());
  CHECK(recovering.volume->GetClusterSize() == strict.volume->GetClusterSize());
}

TEMPLATE_TEST_CASE_SIG("an unknown volume size gives no backup",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened<S> opened =
      Open<S>(Image(), VolumeOptions{.recover_errors = true}, std::nullopt);

  CHECK_FALSE(opened.volume->IsVolumeOK());
  CHECK(opened.record->size_calls == 1);
  CHECK(opened.BackupReads() == 0);
  CHECK_THAT(opened.log, ContainsSubstring(no_size_message));
}

TEMPLATE_TEST_CASE_SIG("a volume size under one boot sector gives no backup",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const ULONGLONG size :
       {ULONGLONG{0}, ULONGLONG{boot_sector_bytes - 1}}) {
    INFO("size = " << size);
    const Opened<S> opened =
        Open<S>(Image(), VolumeOptions{.recover_errors = true}, size);

    CHECK_FALSE(opened.volume->IsVolumeOK());
    CHECK(opened.BackupReads() == 0);
    CHECK_THAT(opened.log, ContainsSubstring(no_size_message));
  }
}

TEMPLATE_TEST_CASE_SIG("a size that is no multiple of a sector gives no backup",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // 100 is a multiple of none of the sector sizes: no candidate is read.
  std::vector<BYTE> image = Image();
  const ULONGLONG size = image.size() + 100;
  const Opened<S> opened =
      Open<S>(std::move(image), VolumeOptions{.recover_errors = true}, size);

  CHECK_FALSE(opened.volume->IsVolumeOK());
  CHECK(opened.BackupReads() == 0);
  CHECK_THAT(opened.log, ContainsSubstring(no_candidate_message));
}

TEMPLATE_TEST_CASE_SIG("a size that fits only a smaller sector skips the rest",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // A multiple of 512 but not of 1024: only the 512 candidate applies.
  const size_t image_bytes = Tests::fake_backup_image_bytes + boot_sector_bytes;
  const Opened<S> opened =
      Open<S>(Tests::BuildFakeNtfsImageWithBackupBootSector(
                  {.image_bytes = image_bytes}),
              true);

  CHECK(opened.volume->IsVolumeOK());
  for (const LONGLONG start : opened.record->starts) {
    CHECK(start != static_cast<LONGLONG>(image_bytes - 1024));
    CHECK(start != static_cast<LONGLONG>(image_bytes - 4096));
  }
}

TEMPLATE_TEST_CASE_SIG("a candidate must declare the sector size it is read at",
                       "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  struct Case {
    const char* name;
    WORD declared;
    WORD placed_for;
  };

  for (const Case& mismatch :
       {Case{"a 1024-byte sector copy where a 512 one is read", 1024, 512},
        Case{"a 512-byte sector copy where a 1024 one is read", 512, 1024},
        Case{"a 512-byte sector copy where a 4096 one is read", 512, 4096},
        Case{"a 256-byte sector copy where a 512 one is read", 256, 512}}) {
    INFO(mismatch.name);
    const Opened<S> opened =
        Open<S>(Tests::BuildFakeNtfsImageWithBackupBootSector(
                    {.sector_size = mismatch.declared,
                     .placed_for = mismatch.placed_for}),
                true);

    CHECK_FALSE(opened.volume->IsVolumeOK());
    CHECK_THAT(opened.log, !ContainsSubstring(backup_message));
    CHECK_THAT(opened.log, ContainsSubstring(no_candidate_message));
  }
}
