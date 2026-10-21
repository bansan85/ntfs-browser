#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "corpus-test-support.h"
#include "data/ntfs-bpb.h"
#include "external-mft-support.h"
#include "standalone-mft-disk-reader.h"
#include "test-log-sink.h"

namespace {

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::DfirNtfsDir;
using Tests::ReadCorpusFile;
using Tests::RequireCorpusFile;
using Tests::StandaloneMftDiskReader;

// Logged when a record comes from $MFTMirr.
constexpr std::string_view fallback_message = "using its copy in $MFTMirr";

// Records that $MFTMirr copies, and the names the system files 0 to 3 have on
// every NTFS volume.
constexpr std::array<std::wstring_view, 4> system_file_names = {
    L"$MFT", L"$MFTMirr", L"$LogFile", L"$Volume"};

// Bytes of the boot sector the reader overlays: its BPB.
constexpr std::size_t boot_sector_bytes = 512;

// Offset, in a record, of the last two bytes of its first sector: where the
// update sequence number must be.
constexpr std::size_t first_sector_end = 510;

// How a record is damaged.
enum class Damage : unsigned char {
  // The "FILE" signature is erased.
  Magic,
  // The sequence number at the end of the first sector no longer matches.
  Fixup
};

std::filesystem::path Fixture(std::string_view name) {
  return RequireCorpusFile(DfirNtfsDir() / "test_data" /
                           std::u8string(name.begin(), name.end()));
}

// The real 4 KiB-record $MFT 4k-large.mft, seen as a volume that also holds
// its real $MFTMirr copy 4k-large.mftmirr one cluster past the last cluster of
// $MFT, and whose boot sector points there. Records of $MFT can be damaged on
// the fly. The files are never changed.
class MirroredMftDiskReader final : public IDiskReader {
 public:
  MirroredMftDiskReader(std::unique_ptr<StandaloneMftDiskReader> mft,
                        std::vector<BYTE> mirror,
                        std::vector<std::pair<unsigned, Damage>> damaged)
      : mft_(std::move(mft)),
        mirror_(std::move(mirror)),
        damaged_(std::move(damaged)),
        mft_end_(*mft_->Size()) {
    cluster_size_ = mft_->ClusterSize();
    record_size_ = mft_->FileRecordSize();
    const ULONGLONG mirror_clusters =
        (mirror_.size() + cluster_size_ - 1) / cluster_size_;
    total_bytes_ = mft_end_ + mirror_clusters * cluster_size_;

    boot_.resize(boot_sector_bytes);
    LARGE_INTEGER origin{};
    REQUIRE(mft_->ReadInto(origin, boot_));
    auto& bpb =
        *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(boot_.data());
    mft_addr_ = bpb.lcn_mft * cluster_size_;
    bpb.lcn_mft_mirr = mft_end_ / cluster_size_;
    bpb.total_sectors = total_bytes_ / bpb.bytes_per_sector;
  }

  bool Open(std::wstring_view /*path*/) override { return false; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    if (addr.QuadPart < 0) {
      return false;
    }
    const auto start = static_cast<ULONGLONG>(addr.QuadPart);
    const ULONGLONG end = start + dest.size();
    if (end > total_bytes_) {
      return false;
    }
    if (start >= mft_end_) {
      ++mirror_reads_;
    }

    if (start < mft_end_) {
      LARGE_INTEGER head = addr;
      const std::size_t head_bytes = std::min<ULONGLONG>(end, mft_end_) - start;
      if (!mft_->ReadInto(head, dest.first(head_bytes))) {
        return false;
      }
    }
    for (ULONGLONG at = std::max(start, mft_end_); at < end; ++at) {
      const ULONGLONG in_mirror = at - mft_end_;
      dest[at - start] = in_mirror < mirror_.size() ? mirror_[in_mirror] : 0;
    }

    Overlay(start, boot_, 0, dest);
    for (const auto& [record, damage] : damaged_) {
      const ULONGLONG record_start = mft_addr_ + record * record_size_;
      if (damage == Damage::Magic) {
        const std::array<BYTE, 4> erased = {0, 0, 0, 0};
        Overlay(start, erased, record_start, dest);
      } else {
        BYTE corrupt = 0;
        const ULONGLONG at = record_start + first_sector_end;
        if (at >= start && at < end) {
          corrupt = dest[at - start] ^ 0xFF;
          Overlay(start, std::span<const BYTE>(&corrupt, 1), at, dest);
        }
      }
    }
    return true;
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    return total_bytes_;
  }

  // Reads that started inside the $MFTMirr copy.
  [[nodiscard]] std::size_t MirrorReads() const noexcept {
    return mirror_reads_;
  }

 private:
  // Copies the part of bytes, which sit at address at, that falls inside the
  // read of dest starting at start.
  static void Overlay(ULONGLONG start, std::span<const BYTE> bytes,
                      ULONGLONG at, std::span<BYTE> dest) {
    const ULONGLONG end = start + dest.size();
    const ULONGLONG from = std::max(start, at);
    const ULONGLONG to = std::min(end, at + bytes.size());
    for (ULONGLONG pos = from; pos < to; ++pos) {
      dest[pos - start] = bytes[pos - at];
    }
  }

  std::unique_ptr<StandaloneMftDiskReader> mft_;
  std::vector<BYTE> mirror_;
  std::vector<std::pair<unsigned, Damage>> damaged_;
  ULONGLONG mft_end_;
  DWORD cluster_size_{0};
  DWORD record_size_{0};
  ULONGLONG total_bytes_{0};
  ULONGLONG mft_addr_{0};
  std::vector<BYTE> boot_;
  mutable std::size_t mirror_reads_{0};
};

// An opened volume over the real $MFT and its real $MFTMirr, with what the
// open logged.
template <Cache::Strategy S>
struct Opened {
  std::unique_ptr<NtfsVolume<S>> volume;
  // Owned by volume.
  const MirroredMftDiskReader* reader{nullptr};
  std::string log;
};

template <Cache::Strategy S>
Opened<S> Open(std::vector<std::pair<unsigned, Damage>> damaged, bool recover,
               bool damage_mirror = false) {
  auto mft = std::make_unique<StandaloneMftDiskReader>();
  const bool opened = mft->Open(Fixture("4k-large.mft").wstring());
  INFO("StandaloneMftDiskReader::Open: " << mft->Failure());
  REQUIRE(opened);
  std::vector<BYTE> mirror = ReadCorpusFile(Fixture("4k-large.mftmirr"));
  if (damage_mirror) {
    std::fill_n(mirror.begin(), 4, BYTE{0});
  }

  auto reader = std::make_unique<MirroredMftDiskReader>(
      std::move(mft), std::move(mirror), std::move(damaged));
  Opened<S> result;
  result.reader = reader.get();
  static_cast<void>(Tests::TakeCapturedLog());
  result.volume = std::make_unique<NtfsVolume<S>>(
      std::move(reader), VolumeOptions{.recover_errors = recover});
  result.log = Tests::TakeCapturedLog();
  return result;
}

// The name of record, read through volume.
template <Cache::Strategy S>
std::optional<std::wstring> NameOfRecord(const NtfsVolume<S>& volume,
                                         ULONGLONG record) {
  Io::FileRecord<S> file_record(volume);
  if (!file_record.ParseFileRecord(record) || !file_record.ParseAttrs()) {
    return std::nullopt;
  }
  return std::wstring(file_record.GetFileName());
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("an intact real $MFT never reads its $MFTMirr",
                       "[dfir-corpora][integration][mft-mirror]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  for (const bool recover : {false, true}) {
    INFO("recover_errors = " << recover);
    const Opened<S> opened = Open<S>({}, recover);

    REQUIRE(opened.volume->IsVolumeOK());
    CHECK(opened.reader->MirrorReads() == 0);
    CHECK_THAT(opened.log, !ContainsSubstring(std::string(fallback_message)));
    for (ULONGLONG record = 0; record < system_file_names.size(); ++record) {
      CHECK(NameOfRecord(*opened.volume, record) ==
            std::wstring(system_file_names.at(record)));
    }
    CHECK(opened.reader->MirrorReads() == 0);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "a damaged first record of a real $MFT comes from the "
    "real $MFTMirr copy",
    "[dfir-corpora][integration][mft-mirror]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const Damage damage : {Damage::Magic, Damage::Fixup}) {
    INFO("damage = " << static_cast<int>(damage));

    SECTION("strict") {
      const Opened<S> opened = Open<S>({{0, damage}}, false);

      CHECK_FALSE(opened.volume->IsVolumeOK());
      CHECK(opened.reader->MirrorReads() == 0);
      CHECK_THAT(opened.log, !ContainsSubstring(std::string(fallback_message)));
    }

    SECTION("recovering") {
      const Opened<S> opened = Open<S>({{0, damage}}, true);

      REQUIRE(opened.volume->IsVolumeOK());
      CHECK(opened.reader->MirrorReads() >= 1);
      CHECK_THAT(opened.log, ContainsSubstring(std::string(fallback_message)));
      CHECK(opened.volume->GetFileRecordSize() == 4096);
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "a damaged system record of a real $MFT comes from the "
    "real $MFTMirr copy",
    "[dfir-corpora][integration][mft-mirror]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const unsigned record = GENERATE(1U, 2U, 3U);
  const Damage damage = GENERATE(Damage::Magic, Damage::Fixup);
  INFO("record = " << record << ", damage = " << static_cast<int>(damage));

  SECTION("strict") {
    const Opened<S> opened = Open<S>({{record, damage}}, false);

    // Record 3 is read when the volume opens: $Volume is lost.
    if (record == 3) {
      CHECK_FALSE(opened.volume->IsVolumeOK());
    } else {
      REQUIRE(opened.volume->IsVolumeOK());
      CHECK_FALSE(NameOfRecord(*opened.volume, record).has_value());
    }
    CHECK(opened.reader->MirrorReads() == 0);
  }

  SECTION("recovering") {
    const Opened<S> opened = Open<S>({{record, damage}}, true);

    REQUIRE(opened.volume->IsVolumeOK());
    // The copy is the real record, so the name is the real one.
    CHECK(NameOfRecord(*opened.volume, record) ==
          std::wstring(system_file_names.at(record)));
    CHECK(opened.reader->MirrorReads() >= 1);
  }
}

TEMPLATE_TEST_CASE_SIG("a real $MFTMirr copy that is damaged too saves nothing",
                       "[dfir-corpora][integration][mft-mirror]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  const Opened<S> opened =
      Open<S>({{0, Damage::Magic}}, true, /*damage_mirror=*/true);

  CHECK_FALSE(opened.volume->IsVolumeOK());
  CHECK(opened.reader->MirrorReads() >= 1);
  CHECK_THAT(opened.log, !ContainsSubstring(std::string(fallback_message)));
}

TEST_CASE("The records of a real $MFTMirr copy are the system files 0 to 3",
          "[dfir-corpora][integration][mft-mirror]") {
  // The two copies hold 1 KiB and 4 KiB records.
  const std::string_view copy = GENERATE("boot.mftmirr", "4k-large.mftmirr");
  CAPTURE(copy);
  const auto volume =
      Tests::OpenStandaloneMftVolume(Fixture(copy), VolumeOptions{});

  for (ULONGLONG record = 0; record < system_file_names.size(); ++record) {
    CHECK(NameOfRecord(*volume, record) ==
          std::wstring(system_file_names.at(record)));
  }
}
