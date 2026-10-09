#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "attr-file-name.h"
#include "attr-resident.h"
#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "optional-access.h"
#include "partition-disk-reader.h"

using NtfsBrowser::AttrBase;
namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::Attr::AttrFileName;
using NtfsBrowser::Attr::AttrResidentNoCache;
namespace Mft = NtfsBrowser::Mft;

namespace {

// The "ntfs-samples" forensic test image corpus, documented in that repo's
// ReadMe.md. Each image is checked in there as a gzip/rar archive, too large
// to check into this one too.
const std::filesystem::path& samples_dir = NtfsBrowserTests::NtfsSamplesDir();
const std::filesystem::path ptrn_image = samples_dir / "ntfs-ptrn.raw";
const std::filesystem::path ramslack_image = samples_dir / "ntfs-ramslack.raw";
const std::filesystem::path lastaccess_image =
    samples_dir / "ntfs-lastaccess.raw";
const std::filesystem::path k2m_image = samples_dir / "ntfs-2m.raw";
const std::filesystem::path si_vs_fn_image = samples_dir / "ntfs-si-vs-fn.raw";
// 64 GiB once decompressed: CI leaves ntfs.tgz compressed, and its test skips.
const std::filesystem::path ntfs_image = samples_dir / "ntfs.raw";

// Ticks (100 ns units) in one second: FILETIME's own unit.
constexpr ULONGLONG ticks_per_second = 10'000'000;

// Every ntfs-samples image is a whole-disk image: an MBR partition table
// followed by a single NTFS partition, not a bare volume. These are that
// NTFS partition's own starting byte offset (LBA start x 512), read from
// each image's MBR partition entry (offset 0x1BE) - ntfs-2m.raw,
// ntfs-ptrn.raw, ntfs-ramslack.raw, ntfs-lastaccess.raw and
// ntfs-si-vs-fn.raw share the smaller one; ntfs.raw and
// ntfs_extremely_fragmented_mft.raw the larger, 1 MiB-aligned one.
constexpr ULONGLONG small_image_partition_offset = 65536;

// Byte addresses of /1.txt's RAM slack and cluster slack in the ramslack
// image, from its ReadMe.md.
constexpr LONGLONG ram_slack_address = 213303;
constexpr LONGLONG cluster_slack_address = 213504;

// Gaps, in seconds, between the two timestamps each timestamp test compares,
// from the images' ReadMe.md.
constexpr ULONGLONG lastaccess_delta_seconds = 158;
constexpr ULONGLONG long_mismatch_seconds = 813;
constexpr ULONGLONG short_mismatch_seconds = 120;
constexpr ULONGLONG large_image_partition_offset = 1048576;

// Opens imagePath through a PartitionDiskReader, so NtfsVolume sees the NTFS
// partition's own boot sector at addr 0 instead of the image's MBR.
std::unique_ptr<NtfsBrowser::IDiskReader>
    OpenWholeDiskImage(const std::filesystem::path& image_path,
                       ULONGLONG partition_offset) {
  auto reader =
      std::make_unique<NtfsBrowserTests::PartitionDiskReader>(partition_offset);
  REQUIRE(reader->Open(image_path.wstring()));
  return reader;
}

// Parses dir's own file record as the volume's root directory, ready for
// FindSubEntry(). dir must already be constructed on that volume.
void OpenRootDir(FileRecord<Cache::Strategy::NoCache>& dir) {
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(dir.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(dir.ParseAttrs());
}

// Looks up name under dir's current directory and reparses dir in place as
// that subdirectory.
void OpenSubDir(FileRecord<Cache::Strategy::NoCache>& dir,
                std::wstring_view name) {
  const std::optional<IndexEntry> entry = dir.FindSubEntry(name);
  REQUIRE(entry.has_value());
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(
      dir.ParseFileRecord(NtfsBrowserTests::Unwrap(entry).GetFileReference()));
  REQUIRE(dir.ParseAttrs());
}

// Navigates dir down a directory path, one component at a time, from the
// volume root.
void OpenDirPath(FileRecord<Cache::Strategy::NoCache>& dir,
                 std::initializer_list<std::wstring_view> parts) {
  OpenRootDir(dir);
  for (const std::wstring_view part : parts) {
    OpenSubDir(dir, part);
  }
}

// True if data is entirely the repeating 4-byte "PTRN" pattern these corpus
// images were filled with before formatting, starting at whatever phase of
// the pattern data[0] happens to land on.
bool MatchesPtrnPattern(std::span<const BYTE> data) {
  constexpr std::string_view pattern_value = "PTRN";
  if (data.empty()) {
    return false;
  }

  // data is not empty: checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const size_t phase = pattern_value.find(static_cast<char>(data[0]));
  if (phase == std::string_view::npos) {
    return false;
  }

  for (size_t i = 0; i < data.size(); i++) {
    // i < data.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (data[i] !=
        gsl::narrow<BYTE>(pattern_value[(phase + i) % pattern_value.size()])) {
      return false;
    }
  }
  return true;
}

// True if data contains an uninterrupted run of at least minRunLength bytes
// cycling through the "PTRN" pattern, starting at whatever phase that run
// happens to land on.
bool ContainsPtrnRun(std::span<const BYTE> data, size_t min_run_length) {
  constexpr std::string_view pattern_value = "PTRN";
  size_t position = 0;
  while (position < data.size()) {
    // i < data.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const size_t phase = pattern_value.find(static_cast<char>(data[position]));
    if (phase == std::string_view::npos) {
      position++;
      continue;
    }

    size_t run_end = position;
    while (run_end < data.size()) {
      const auto expected = gsl::narrow<BYTE>(
          // The index is reduced modulo pattern.size().
          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
          pattern_value[(phase + (run_end - position)) % pattern_value.size()]);
      // j < data.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      if (data[run_end] != expected) {
        break;
      }
      run_end++;
    }
    if (run_end - position >= min_run_length) {
      return true;
    }
    position = run_end > position ? run_end : position + 1;
  }
  return false;
}

// $Extend\$RmMetadata\$Repair's "$Corrupt" and "$Verify" streams: a short
// real header, then space the transaction-repair mechanism marks allocated
// but never actually wrote to, still holding the "PTRN" pattern underneath.
// Shared by ntfs-ptrn.raw, ntfs-ramslack.raw and ntfs-lastaccess.raw, all
// filled with that pattern before formatting.
void CheckRepairStreamsHoldPtrnPattern(
    const NtfsVolume<Cache::Strategy::NoCache>& volume) {
  FileRecord<Cache::Strategy::NoCache> dir(volume);
  OpenDirPath(dir, {L"$Extend", L"$RmMetadata"});

  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"$Repair");
  REQUIRE(entry.has_value());

  FileRecord<Cache::Strategy::NoCache> file(volume);
  file.SetAttrMask(Attr::Mask::Data);
  REQUIRE(
      file.ParseFileRecord(NtfsBrowserTests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());

  for (const std::wstring_view stream_name : {L"$Corrupt", L"$Verify"}) {
    const AttrBase<Cache::Strategy::NoCache>* stream =
        file.FindStream(stream_name);
    REQUIRE(stream != nullptr);

    std::vector<BYTE> data(stream->GetDataSize());
    REQUIRE(stream->ReadData(0, data) == data.size());
    // One page: comfortably below the multi-hundred-KB run actually present,
    // and long enough that a false positive by chance is not a real concern.
    CHECK(ContainsPtrnRun(data, 4096));
  }
}

// Absolute difference between two FILETIMEs, in 100 ns ticks. Comparing raw
// ticks instead of converting to SYSTEMTIME sidesteps GetFileTime()'s
// UTC-to-local conversion (AttrStdInfo::UTC2Local), which depends on the
// test machine's own timezone and would otherwise make an exact wall-clock
// comparison flaky.
ULONGLONG TickDelta(const FILETIME& first, const FILETIME& second) {
  const ULONGLONG first_ticks = NtfsBrowserTests::FileTimeToTicks(first);
  const ULONGLONG second_ticks = NtfsBrowserTests::FileTimeToTicks(second);
  return first_ticks > second_ticks ? first_ticks - second_ticks
                                    : second_ticks - first_ticks;
}

// ReadMe.md documents each timestamp only to the whole second, so each one's
// true sub-second remainder is unknown; the true delta can therefore land
// anywhere in the open, one-second-wide margin around expectedSeconds.
void CheckDeltaMatchesSeconds(ULONGLONG delta_ticks,
                              ULONGLONG expected_seconds) {
  CHECK(delta_ticks > (expected_seconds - 1) * ticks_per_second);
  CHECK(delta_ticks < (expected_seconds + 1) * ticks_per_second);
}

// Year/month/day of ft, converted from its (local-time) FILETIME.
std::tuple<WORD, WORD, WORD> ToDate(const FILETIME& file_time) {
  return NtfsBrowserTests::FileTimeToDate(file_time);
}

}  // namespace

TEST_CASE("Opens a volume with 2 MiB clusters (ntfs-2m.raw)",
          "[ntfs-samples][integration]") {
  NtfsBrowserTests::RequireCorpusImage(k2m_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      OpenWholeDiskImage(k2m_image, small_image_partition_offset));
  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetClusterSize() == 2097152);

  FileRecord<Cache::Strategy::NoCache> root(volume);
  OpenRootDir(root);

  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);
  CHECK_FALSE(names.empty());
}

TEST_CASE("Reads /2.txt and finds the $Repair PTRN artifact (ntfs-ptrn.raw)",
          "[ntfs-samples][integration][slack]") {
  NtfsBrowserTests::RequireCorpusImage(ptrn_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      OpenWholeDiskImage(ptrn_image, small_image_partition_offset));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Cache::Strategy::NoCache> root(volume);
  OpenRootDir(root);
  const std::optional<IndexEntry> entry = root.FindSubEntry(L"2.txt");
  REQUIRE(entry.has_value());
  CHECK(NtfsBrowserTests::Unwrap(entry).GetFileSize() > 0);

  // /2.txt's own cluster slack (past its real size, still inside its last
  // allocated cluster) isn't checked: ReadData() clamps every read to
  // real_size (AttrNonResident::ReadData), and the ReadMe gives no absolute
  // offset for it here (unlike ntfs-ramslack.raw, below) to read around that.
  CheckRepairStreamsHoldPtrnPattern(volume);
}

TEST_CASE(
    "Finds the RAM slack and cluster slack PTRN pattern (ntfs-ramslack.raw)",
    "[ntfs-samples][integration][slack]") {
  NtfsBrowserTests::RequireCorpusImage(ramslack_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      OpenWholeDiskImage(ramslack_image, small_image_partition_offset));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Cache::Strategy::NoCache> root(volume);
  OpenRootDir(root);
  REQUIRE(root.FindSubEntry(L"1.txt").has_value());

  // RAM slack: the tail of /1.txt's last written sector, past its own real
  // size but still inside the sector Windows wrote, holding whatever was on
  // disk before (the "PTRN" pattern).
  LARGE_INTEGER ram_slack_addr{.QuadPart = ram_slack_address};
  const std::optional<std::span<const BYTE>> ram_slack =
      volume.Read(ram_slack_addr, 4);
  REQUIRE(ram_slack.has_value());
  CHECK(MatchesPtrnPattern(NtfsBrowserTests::Unwrap(ram_slack)));

  // Cluster slack: the rest of the cluster past that same sector.
  LARGE_INTEGER cluster_slack_addr{.QuadPart = cluster_slack_address};
  const std::optional<std::span<const BYTE>> cluster_slack =
      volume.Read(cluster_slack_addr, 4);
  REQUIRE(cluster_slack.has_value());
  CHECK(MatchesPtrnPattern(NtfsBrowserTests::Unwrap(cluster_slack)));

  CheckRepairStreamsHoldPtrnPattern(volume);
}

TEST_CASE(
    "STANDARD_INFORMATION and $I30 FILE_NAME disagree on /test/1.txt's last "
    "access time (ntfs-lastaccess.raw)",
    "[ntfs-samples][integration][timestamps]") {
  NtfsBrowserTests::RequireCorpusImage(lastaccess_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      OpenWholeDiskImage(lastaccess_image, small_image_partition_offset));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Cache::Strategy::NoCache> dir(volume);
  OpenDirPath(dir, {L"test"});
  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"1.txt");
  REQUIRE(entry.has_value());

  FILETIME index_access{};
  NtfsBrowserTests::Unwrap(entry).GetFileTime(nullptr, nullptr, &index_access);

  FileRecord<Cache::Strategy::NoCache> file(volume);
  file.SetAttrMask(Attr::Mask::StandardInformation);
  REQUIRE(
      file.ParseFileRecord(NtfsBrowserTests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());

  FILETIME std_info_access{};
  file.GetFileTime(nullptr, nullptr, &std_info_access);

  // ReadMe.md: 2019-03-03 12:37:55 ($STANDARD_INFORMATION) vs.
  // 2019-03-03 12:35:17 ($I30 FILE_NAME) - 2 min 38 s apart.
  CheckDeltaMatchesSeconds(TickDelta(std_info_access, index_access),
                           lastaccess_delta_seconds);

  CheckRepairStreamsHoldPtrnPattern(volume);
}

TEST_CASE(
    "STANDARD_INFORMATION, FILE_NAME and $I30 FILE_NAME each carry a "
    "different creation date for /test/test.txt (ntfs-si-vs-fn.raw)",
    "[ntfs-samples][integration][timestamps]") {
  NtfsBrowserTests::RequireCorpusImage(si_vs_fn_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      OpenWholeDiskImage(si_vs_fn_image, small_image_partition_offset));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Cache::Strategy::NoCache> dir(volume);
  OpenDirPath(dir, {L"test"});
  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"test.txt");
  REQUIRE(entry.has_value());

  FILETIME index_create{};
  NtfsBrowserTests::Unwrap(entry).GetFileTime(nullptr, &index_create, nullptr);

  FileRecord<Cache::Strategy::NoCache> file(volume);
  file.SetAttrMask(Attr::Mask::StandardInformation | Attr::Mask::FileName);
  REQUIRE(
      file.ParseFileRecord(NtfsBrowserTests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());

  FILETIME std_info_create{};
  file.GetFileTime(nullptr, &std_info_create, nullptr);

  // The file record's own $FILE_NAME attribute: not the parent directory's
  // $I30 copy above, but the (possibly stale) one alongside this file's own
  // $STANDARD_INFORMATION. AttrFileName<>::GetFileTime() is the same,
  // otherwise-unreachable Filename::GetFileTime() IndexEntry uses; matches
  // FileRecord::GetFileTime()'s own internal cast (src/file-record.cpp).
  const auto& file_name_attrs = file.GetAttr(Attr::Type::FileName);
  REQUIRE_FALSE(file_name_attrs.empty());
  const auto* own_file_name = reinterpret_cast<
      const AttrFileName<AttrResidentNoCache, Cache::Strategy::NoCache>*>(
      file_name_attrs.front().get());
  FILETIME own_file_name_create{};
  own_file_name->GetFileTime(nullptr, &own_file_name_create, nullptr);

  // ReadMe.md: the file record's own $FILE_NAME says 2014-12-12, its
  // $STANDARD_INFORMATION says 2015-11-03, and the parent directory's $I30
  // $FILE_NAME says 2016-09-24.
  CHECK(ToDate(own_file_name_create) ==
        std::tuple<WORD, WORD, WORD>{2014, 12, 12});
  CHECK(ToDate(std_info_create) == std::tuple<WORD, WORD, WORD>{2015, 11, 3});
  CHECK(ToDate(index_create) == std::tuple<WORD, WORD, WORD>{2016, 9, 24});
}

TEST_CASE(
    "STANDARD_INFORMATION and $I30 FILE_NAME disagree on two files' last "
    "access time (ntfs.raw)",
    "[ntfs-samples][integration][timestamps]") {
  if (!std::filesystem::exists(ntfs_image)) {
    SKIP("ntfs.raw not present: " << ntfs_image.string());
  }

  // This image also documents two VSS shadow copies (one hidden from
  // "vssadmin list shadows") and a stray $I30 entry surviving in $MFT record
  // slack space. Neither is checked here: this library has no VSS support,
  // and ParseFileRecord()/ParseAttrs() only ever read a record's real,
  // in-use attribute area, never its unused slack bytes.
  NtfsVolume<Cache::Strategy::NoCache> volume(
      OpenWholeDiskImage(ntfs_image, large_image_partition_offset));
  REQUIRE(volume.IsVolumeOK());

  const auto check_access_time_mismatch =
      [&volume](std::wstring_view dir_name, std::wstring_view file_name,
                ULONGLONG expected_delta_seconds) {
        FileRecord<Cache::Strategy::NoCache> dir(volume);
        OpenDirPath(dir, {dir_name});
        const std::optional<IndexEntry> entry = dir.FindSubEntry(file_name);
        REQUIRE(entry.has_value());

        FILETIME index_access{};
        entry->GetFileTime(nullptr, nullptr, &index_access);

        FileRecord<Cache::Strategy::NoCache> file(volume);
        file.SetAttrMask(Attr::Mask::StandardInformation);
        REQUIRE(file.ParseFileRecord(entry->GetFileReference()));
        REQUIRE(file.ParseAttrs());

        FILETIME std_info_access{};
        file.GetFileTime(nullptr, nullptr, &std_info_access);

        CheckDeltaMatchesSeconds(TickDelta(std_info_access, index_access),
                                 expected_delta_seconds);
      };

  // ReadMe.md: 2020-07-25 12:49:21 vs. 12:35:48 - 13 min 33 s apart.
  check_access_time_mismatch(L"test_dir_2", L"file_2_1.txt",
                             long_mismatch_seconds);
  // ReadMe.md: 2020-07-25 12:33:24 vs. 12:35:24 - 2 min apart.
  check_access_time_mismatch(L"test_dir", L"file_1.txt",
                             short_mismatch_seconds);
}

TEST_CASE(
    "Resolves every file record of an extremely fragmented $MFT "
    "(ntfs_extremely_fragmented_mft.raw)",
    "[ntfs-samples][integration][fragmented-mft]") {
  const std::filesystem::path& image = NtfsBrowserTests::FragmentedMftImage();
  if (!std::filesystem::exists(image)) {
    SKIP("ntfs_extremely_fragmented_mft.raw not present: " << image.string());
  }

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      OpenWholeDiskImage(image, large_image_partition_offset));
  REQUIRE(volume.IsVolumeOK());

  // ReadMe.md: these are the file records ($MFT's own base record, plus its
  // extension records) whose attributes make up $MFT's own, heavily
  // fragmented $DATA run list. Resolving each one exercises that fragmented
  // run list, through NtfsVolume::ReadMftData()/mft_extents_.
  for (const ULONGLONG record_num :
       {0ULL, 15ULL, 16ULL, 17ULL, 18ULL, 19ULL, 20ULL, 21ULL, 22ULL,
        34'799'617ULL, 34'799'618ULL, 34'799'619ULL}) {
    INFO("record " << record_num);
    FileRecord<Cache::Strategy::NoCache> record(volume);
    REQUIRE(record.ParseFileRecord(record_num));
    CHECK(record.GetBaseRecordReference() == 0);
  }
}
