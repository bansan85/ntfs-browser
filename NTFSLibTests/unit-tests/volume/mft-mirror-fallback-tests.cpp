#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <cstring>
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
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "catch2/catch_tostring.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "data/ntfs-bpb.h"
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
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// Logged when a record comes from $MFTMirr.
constexpr const char* fallback_message = "using its copy in $MFTMirr";

// Start of the $MFTMirr copy in the fake image, in bytes.
constexpr LONGLONG mirror_addr = static_cast<LONGLONG>(
    Tests::fake_mft_mirror_lcn * Tests::fake_cluster_size);

// Bytes the copy spans.
constexpr LONGLONG mirror_bytes = static_cast<LONGLONG>(
    Tests::fake_mft_mirror_records * Tests::fake_file_record_size);

// LCN of $MFT in the fake image: right after the boot sector's cluster.
constexpr ULONGLONG fake_mft_lcn = 1;

// A volume whose 128 sectors end at LCN 64: a copy starting here no longer
// fits.
constexpr ULONGLONG lcn_past_the_volume = 61;

// Bit of a record number in a Tests::MftMirrorDamage mask.
constexpr std::uint32_t Bit(unsigned record) { return 1U << record; }

// A MemoryDiskReader that remembers where each read started.
class SpyDiskReader final : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  SpyDiskReader(std::vector<BYTE> image,
                std::shared_ptr<std::vector<LONGLONG>> starts)
      : inner_(std::move(image)), starts_(std::move(starts)) {}

  bool Open(std::wstring_view /*path*/) override { return false; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    starts_->push_back(addr.QuadPart);
    return inner_.ReadInto(addr, dest);
  }

 private:
  Tests::MemoryDiskReader inner_;
  std::shared_ptr<std::vector<LONGLONG>> starts_;
};

// An opened volume, with what the open read and logged.
template <Cache::Strategy S>
struct Opened {
  std::unique_ptr<NtfsVolume<S>> volume;
  std::shared_ptr<std::vector<LONGLONG>> starts;
  std::string log;

  // Reads that started inside the $MFTMirr copy.
  [[nodiscard]] size_t MirrorReads() const {
    size_t count = 0;
    for (const LONGLONG start : *starts) {
      count += start >= mirror_addr && start < mirror_addr + mirror_bytes;
    }
    return count;
  }
};

template <Cache::Strategy S>
Opened<S> Open(std::vector<BYTE> image, bool recover) {
  (void)Tests::TakeCapturedLog();
  auto starts = std::make_shared<std::vector<LONGLONG>>();
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<SpyDiskReader>(std::move(image), starts),
      VolumeOptions{.recover_errors = recover});
  return {.volume = std::move(volume),
          .starts = std::move(starts),
          .log = Tests::TakeCapturedLog()};
}

// BuildFakeNtfsImageWithMftMirror() with the given damage.
std::vector<BYTE>
    Image(std::uint32_t in_mft, std::uint32_t in_mirror = 0,
          Tests::MirrorRecordDamage kind = Tests::MirrorRecordDamage::Magic) {
  return Tests::BuildFakeNtfsImageWithMftMirror(
      {.in_mft = in_mft, .in_mirror = in_mirror, .kind = kind});
}

// The image with its BPB's lcn_mft_mirr replaced.
std::vector<BYTE> ImageWithMirrorLcn(std::uint32_t in_mft,
                                     ULONGLONG lcn_mft_mirr) {
  std::vector<BYTE> image = Image(in_mft);
  reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data())
      ->lcn_mft_mirr = lcn_mft_mirr;
  return image;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("a strict volume never reads $MFTMirr",
                       "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const std::uint32_t damaged : {Bit(0), Bit(3), Bit(0) | Bit(3)}) {
    INFO("damaged records = " << damaged);
    const Opened<S> opened = Open<S>(Image(damaged), false);

    CHECK_FALSE(opened.volume->IsVolumeOK());
    CHECK(opened.MirrorReads() == 0);
    CHECK_THAT(opened.log, !ContainsSubstring(fallback_message));
  }
}

TEMPLATE_TEST_CASE_SIG("an intact $MFT never reads $MFTMirr",
                       "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const bool recover : {false, true}) {
    INFO("recover_errors = " << recover);
    const Opened<S> opened = Open<S>(Image(0), recover);

    CHECK(opened.volume->IsVolumeOK());
    CHECK(opened.MirrorReads() == 0);
    CHECK_THAT(opened.log, !ContainsSubstring(fallback_message));
  }
}

TEMPLATE_TEST_CASE_SIG("a recovering volume falls back on $MFTMirr",
                       "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const auto kind : {Tests::MirrorRecordDamage::Magic,
                          Tests::MirrorRecordDamage::UpdateSequence}) {
    INFO("damage kind = " << static_cast<int>(kind));

    SECTION("$MFT's own record") {
      const Opened<S> opened = Open<S>(Image(Bit(0), 0, kind), true);

      CHECK(opened.volume->IsVolumeOK());
      CHECK(opened.MirrorReads() >= 1);
      CHECK_THAT(opened.log, ContainsSubstring(fallback_message));
    }

    SECTION("$Volume's record") {
      const Opened<S> opened = Open<S>(Image(Bit(3), 0, kind), true);

      CHECK(opened.volume->IsVolumeOK());
      CHECK(opened.MirrorReads() == 1);
      CHECK_THAT(opened.log, ContainsSubstring(fallback_message));
    }

    SECTION("both records") {
      const Opened<S> opened = Open<S>(Image(Bit(0) | Bit(3), 0, kind), true);

      CHECK(opened.volume->IsVolumeOK());
      CHECK(opened.MirrorReads() >= 2);
      CHECK_THAT(opened.log, ContainsSubstring(fallback_message));
    }
  }
}

TEMPLATE_TEST_CASE_SIG("a recovering volume needs a sound $MFTMirr copy",
                       "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const auto kind : {Tests::MirrorRecordDamage::Magic,
                          Tests::MirrorRecordDamage::UpdateSequence}) {
    INFO("damage kind = " << static_cast<int>(kind));

    SECTION("$MFT's record is lost in both") {
      const Opened<S> opened = Open<S>(Image(Bit(0), Bit(0), kind), true);

      CHECK_FALSE(opened.volume->IsVolumeOK());
      CHECK(opened.MirrorReads() == 1);
      CHECK_THAT(opened.log, !ContainsSubstring(fallback_message));
    }

    SECTION("$Volume's record is lost in both") {
      const Opened<S> opened = Open<S>(Image(Bit(3), Bit(3), kind), true);

      CHECK_FALSE(opened.volume->IsVolumeOK());
      CHECK(opened.MirrorReads() == 1);
      CHECK_THAT(opened.log, !ContainsSubstring(fallback_message));
    }

    SECTION("only the copy of another record is damaged") {
      const Opened<S> opened = Open<S>(Image(Bit(0), Bit(3), kind), true);

      CHECK(opened.volume->IsVolumeOK());
    }
  }
}

TEMPLATE_TEST_CASE_SIG("an unusable $MFTMirr address gives no fallback",
                       "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  struct Case {
    const char* name;
    ULONGLONG lcn;
  };

  for (const Case& unusable :
       {Case{"null", 0}, Case{"the address of $MFT", fake_mft_lcn},
        Case{"overflowing", ~ULONGLONG{0}}, Case{"too high", 1ULL << 53U},
        Case{"past the volume", lcn_past_the_volume}}) {
    INFO(unusable.name);
    for (const std::uint32_t damaged : {Bit(0), Bit(3)}) {
      INFO("damaged records = " << damaged);
      const Opened<S> opened =
          Open<S>(ImageWithMirrorLcn(damaged, unusable.lcn), true);

      CHECK_FALSE(opened.volume->IsVolumeOK());
      CHECK(opened.MirrorReads() == 0);
      CHECK_THAT(opened.log, !ContainsSubstring(fallback_message));
    }
  }
}

TEMPLATE_TEST_CASE_SIG("records 1 and 2 fall back on $MFTMirr too",
                       "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const unsigned record : {1U, 2U}) {
    INFO("record = " << record);

    SECTION("strict") {
      const Opened<S> opened = Open<S>(Image(Bit(record)), false);
      REQUIRE(opened.volume->IsVolumeOK());

      Io::FileRecord<S> file_record(*opened.volume);
      CHECK_FALSE(file_record.ParseFileRecord(record));
      CHECK(opened.MirrorReads() == 0);
    }

    SECTION("recovering") {
      const Opened<S> opened = Open<S>(Image(Bit(record)), true);
      REQUIRE(opened.volume->IsVolumeOK());

      (void)Tests::TakeCapturedLog();
      Io::FileRecord<S> file_record(*opened.volume);
      CHECK(file_record.ParseFileRecord(record));
      CHECK(file_record.GetFileReference() == record);
      CHECK(opened.MirrorReads() == 1);
      CHECK_THAT(Tests::TakeCapturedLog(), ContainsSubstring(fallback_message));
    }

    SECTION("recovering, with the copy damaged too") {
      const Opened<S> opened = Open<S>(Image(Bit(record), Bit(record)), true);
      REQUIRE(opened.volume->IsVolumeOK());

      Io::FileRecord<S> file_record(*opened.volume);
      CHECK_FALSE(file_record.ParseFileRecord(record));
      CHECK(opened.MirrorReads() == 1);
    }
  }
}

TEMPLATE_TEST_CASE_SIG("a sound record read after the open skips $MFTMirr",
                       "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened<S> opened = Open<S>(Image(0), true);
  REQUIRE(opened.volume->IsVolumeOK());

  for (const ULONGLONG record : {0ULL, 1ULL, 2ULL, 3ULL}) {
    INFO("record = " << record);
    Io::FileRecord<S> file_record(*opened.volume);
    CHECK(file_record.ParseFileRecord(record));
    CHECK(file_record.GetFileReference() == record);
  }
  CHECK(opened.MirrorReads() == 0);
}

TEMPLATE_TEST_CASE_SIG("records past the first four have no fallback",
                       "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Opened<S> opened = Open<S>(Image(0), true);
  REQUIRE(opened.volume->IsVolumeOK());

  // Record 4 is zero-filled in the fake image.
  Io::FileRecord<S> file_record(*opened.volume);
  CHECK_FALSE(file_record.ParseFileRecord(4));
  CHECK(opened.MirrorReads() == 0);
}

TEMPLATE_TEST_CASE_SIG("an image without a mirror opens as before",
                       "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const bool recover : {false, true}) {
    INFO("recover_errors = " << recover);

    SECTION("an intact volume") {
      const Opened<S> opened = Open<S>(Tests::BuildFakeNtfsImage(), recover);

      CHECK(opened.volume->IsVolumeOK());
    }

    SECTION("a lost $MFT record") {
      const Opened<S> opened =
          Open<S>(Tests::BuildFakeNtfsImageWithCorruptMftRecord(), recover);

      CHECK_FALSE(opened.volume->IsVolumeOK());
      CHECK_THAT(opened.log, !ContainsSubstring(fallback_message));
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "a $MFTMirr copy whose header is forged is refused, not trusted",
    "[ntfs-volume][mft-mirror]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // The header field that tells where the update sequence array starts.
  constexpr size_t offset_of_us_field = 4;
  // An offset far past the end of a record.
  constexpr WORD forged_offset = 0xFFFF;
  constexpr size_t volume_record = 3;

  std::vector<BYTE> image = Image(Bit(volume_record));
  const size_t copy_offset = static_cast<size_t>(mirror_addr) +
                             (volume_record * Tests::fake_file_record_size) +
                             offset_of_us_field;
  std::memcpy(&image.at(copy_offset), &forged_offset, sizeof(forged_offset));

  const Opened<S> opened = Open<S>(std::move(image), true);

  CHECK_FALSE(opened.volume->IsVolumeOK());
  CHECK_THAT(opened.log, !ContainsSubstring(fallback_message));
  CHECK_THAT(opened.log, ContainsSubstring("Offset must be lower than"));
}
