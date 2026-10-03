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
#include "partition-disk-reader.h"

using NtfsBrowser::AttrBase;
using NtfsBrowser::AttrFileName;
using NtfsBrowser::AttrResidentNoCache;
using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// The "ntfs-samples" forensic test image corpus, documented in that repo's
// ReadMe.md. Each image is checked in there as a gzip/rar archive, too large
// to check into this one too.
const std::filesystem::path& kSamplesDir = NtfsBrowserTests::NtfsSamplesDir();
const std::filesystem::path kPtrnImage = kSamplesDir / "ntfs-ptrn.raw";
const std::filesystem::path kRamslackImage = kSamplesDir / "ntfs-ramslack.raw";
const std::filesystem::path kLastaccessImage =
    kSamplesDir / "ntfs-lastaccess.raw";
const std::filesystem::path k2mImage = kSamplesDir / "ntfs-2m.raw";
const std::filesystem::path kSiVsFnImage = kSamplesDir / "ntfs-si-vs-fn.raw";
// 64 GiB once decompressed: CI leaves ntfs.tgz compressed, and its test skips.
const std::filesystem::path kNtfsImage = kSamplesDir / "ntfs.raw";

// Ticks (100 ns units) in one second: FILETIME's own unit.
constexpr ULONGLONG kTicksPerSecond = 10'000'000;

// Every ntfs-samples image is a whole-disk image: an MBR partition table
// followed by a single NTFS partition, not a bare volume. These are that
// NTFS partition's own starting byte offset (LBA start x 512), read from
// each image's MBR partition entry (offset 0x1BE) - ntfs-2m.raw,
// ntfs-ptrn.raw, ntfs-ramslack.raw, ntfs-lastaccess.raw and
// ntfs-si-vs-fn.raw share the smaller one; ntfs.raw and
// ntfs_extremely_fragmented_mft.raw the larger, 1 MiB-aligned one.
constexpr ULONGLONG kSmallImagePartitionOffset = 65536;

// Byte addresses of /1.txt's RAM slack and cluster slack in the ramslack
// image, from its ReadMe.md.
constexpr LONGLONG kRamSlackAddress = 213303;
constexpr LONGLONG kClusterSlackAddress = 213504;

// Gaps, in seconds, between the two timestamps each timestamp test compares,
// from the images' ReadMe.md.
constexpr ULONGLONG kLastaccessDeltaSeconds = 158;
constexpr ULONGLONG kLongMismatchSeconds = 813;
constexpr ULONGLONG kShortMismatchSeconds = 120;
constexpr ULONGLONG kLargeImagePartitionOffset = 1048576;

// Opens imagePath through a PartitionDiskReader, so NtfsVolume sees the NTFS
// partition's own boot sector at addr 0 instead of the image's MBR.
std::unique_ptr<NtfsBrowser::IDiskReader>
    OpenWholeDiskImage(const std::filesystem::path& imagePath,
                       ULONGLONG partitionOffset)
{
  auto reader =
      std::make_unique<NtfsBrowserTests::PartitionDiskReader>(partitionOffset);
  REQUIRE(reader->Open(imagePath.wstring()));
  return reader;
}

// Parses dir's own file record as the volume's root directory, ready for
// FindSubEntry(). dir must already be constructed on that volume.
void OpenRootDir(FileRecord<Strategy::NO_CACHE>& dir)
{
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
  REQUIRE(dir.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(dir.ParseAttrs());
}

// Looks up name under dir's current directory and reparses dir in place as
// that subdirectory.
void OpenSubDir(FileRecord<Strategy::NO_CACHE>& dir, std::wstring_view name)
{
  const std::optional<IndexEntry> entry = dir.FindSubEntry(name);
  REQUIRE(entry.has_value());
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
  REQUIRE(dir.ParseFileRecord(entry->GetFileReference()));
  REQUIRE(dir.ParseAttrs());
}

// Navigates dir down a directory path, one component at a time, from the
// volume root.
void OpenDirPath(FileRecord<Strategy::NO_CACHE>& dir,
                 std::initializer_list<std::wstring_view> parts)
{
  OpenRootDir(dir);
  for (std::wstring_view const part : parts)
  {
    OpenSubDir(dir, part);
  }
}

// True if data is entirely the repeating 4-byte "PTRN" pattern these corpus
// images were filled with before formatting, starting at whatever phase of
// the pattern data[0] happens to land on.
bool MatchesPtrnPattern(std::span<const BYTE> data)
{
  constexpr std::string_view kPattern = "PTRN";
  if (data.empty())
  {
    return false;
  }

  // data is not empty: checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const size_t phase = kPattern.find(static_cast<char>(data[0]));
  if (phase == std::string_view::npos)
  {
    return false;
  }

  for (size_t i = 0; i < data.size(); i++)
  {
    // i < data.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (data[i] != gsl::narrow<BYTE>(kPattern[(phase + i) % kPattern.size()]))
    {
      return false;
    }
  }
  return true;
}

// True if data contains an uninterrupted run of at least minRunLength bytes
// cycling through the "PTRN" pattern, starting at whatever phase that run
// happens to land on.
bool ContainsPtrnRun(std::span<const BYTE> data, size_t minRunLength)
{
  constexpr std::string_view kPattern = "PTRN";
  size_t position = 0;
  while (position < data.size())
  {
    // i < data.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const size_t phase = kPattern.find(static_cast<char>(data[position]));
    if (phase == std::string_view::npos)
    {
      position++;
      continue;
    }

    size_t run_end = position;
    while (run_end < data.size())
    {
      const auto expected =
          // The index is reduced modulo kPattern.size().
          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
          gsl::narrow<BYTE>(
              kPattern[(phase + (run_end - position)) % kPattern.size()]);
      // j < data.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      if (data[run_end] != expected)
      {
        break;
      }
      run_end++;
    }
    if (run_end - position >= minRunLength)
    {
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
    const NtfsVolume<Strategy::NO_CACHE>& volume)
{
  FileRecord<Strategy::NO_CACHE> dir(volume);
  OpenDirPath(dir, {L"$Extend", L"$RmMetadata"});

  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"$Repair");
  REQUIRE(entry.has_value());

  FileRecord<Strategy::NO_CACHE> file(volume);
  file.SetAttrMask(Mask::DATA);
  REQUIRE(file.ParseFileRecord(entry->GetFileReference()));
  REQUIRE(file.ParseAttrs());

  for (std::wstring_view const stream_name : {L"$Corrupt", L"$Verify"})
  {
    const AttrBase<Strategy::NO_CACHE>* stream = file.FindStream(stream_name);
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
ULONGLONG TickDelta(const FILETIME& first, const FILETIME& second)
{
  const ULONGLONG first_ticks = NtfsBrowserTests::FileTimeToTicks(first);
  const ULONGLONG second_ticks = NtfsBrowserTests::FileTimeToTicks(second);
  return first_ticks > second_ticks ? first_ticks - second_ticks
                                    : second_ticks - first_ticks;
}

// ReadMe.md documents each timestamp only to the whole second, so each one's
// true sub-second remainder is unknown; the true delta can therefore land
// anywhere in the open, one-second-wide margin around expectedSeconds.
void CheckDeltaMatchesSeconds(ULONGLONG deltaTicks, ULONGLONG expectedSeconds)
{
  CHECK(deltaTicks > (expectedSeconds - 1) * kTicksPerSecond);
  CHECK(deltaTicks < (expectedSeconds + 1) * kTicksPerSecond);
}

// Year/month/day of ft, converted from its (local-time) FILETIME.
std::tuple<WORD, WORD, WORD> ToDate(const FILETIME& file_time)
{
  return NtfsBrowserTests::FileTimeToDate(file_time);
}

}  // namespace

TEST_CASE("Opens a volume with 2 MiB clusters (ntfs-2m.raw)",
          "[ntfs-samples][integration]")
{
  NtfsBrowserTests::RequireCorpusImage(k2mImage);

  NtfsVolume<Strategy::NO_CACHE> const volume(
      OpenWholeDiskImage(k2mImage, kSmallImagePartitionOffset));
  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetClusterSize() == 2'097'152);

  FileRecord<Strategy::NO_CACHE> root(volume);
  OpenRootDir(root);

  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntry& index_entry, void* context)
      {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);
  CHECK_FALSE(names.empty());
}

TEST_CASE("Reads /2.txt and finds the $Repair PTRN artifact (ntfs-ptrn.raw)",
          "[ntfs-samples][integration][slack]")
{
  NtfsBrowserTests::RequireCorpusImage(kPtrnImage);

  NtfsVolume<Strategy::NO_CACHE> const volume(
      OpenWholeDiskImage(kPtrnImage, kSmallImagePartitionOffset));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> root(volume);
  OpenRootDir(root);
  const std::optional<IndexEntry> entry = root.FindSubEntry(L"2.txt");
  REQUIRE(entry.has_value());
  CHECK(entry->GetFileSize() > 0);

  // /2.txt's own cluster slack (past its real size, still inside its last
  // allocated cluster) isn't checked: ReadData() clamps every read to
  // real_size (AttrNonResident::ReadData), and the ReadMe gives no absolute
  // offset for it here (unlike ntfs-ramslack.raw, below) to read around that.
  CheckRepairStreamsHoldPtrnPattern(volume);
}

TEST_CASE(
    "Finds the RAM slack and cluster slack PTRN pattern (ntfs-ramslack.raw)",
    "[ntfs-samples][integration][slack]")
{
  NtfsBrowserTests::RequireCorpusImage(kRamslackImage);

  NtfsVolume<Strategy::NO_CACHE> const volume(
      OpenWholeDiskImage(kRamslackImage, kSmallImagePartitionOffset));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> root(volume);
  OpenRootDir(root);
  REQUIRE(root.FindSubEntry(L"1.txt").has_value());

  // RAM slack: the tail of /1.txt's last written sector, past its own real
  // size but still inside the sector Windows wrote, holding whatever was on
  // disk before (the "PTRN" pattern).
  LARGE_INTEGER ramSlackAddr{.QuadPart = kRamSlackAddress};
  const std::optional<std::span<const BYTE>> ramSlack =
      volume.Read(ramSlackAddr, 4);
  REQUIRE(ramSlack.has_value());
  CHECK(MatchesPtrnPattern(*ramSlack));

  // Cluster slack: the rest of the cluster past that same sector.
  LARGE_INTEGER clusterSlackAddr{.QuadPart = kClusterSlackAddress};
  const std::optional<std::span<const BYTE>> clusterSlack =
      volume.Read(clusterSlackAddr, 4);
  REQUIRE(clusterSlack.has_value());
  CHECK(MatchesPtrnPattern(*clusterSlack));

  CheckRepairStreamsHoldPtrnPattern(volume);
}

TEST_CASE(
    "STANDARD_INFORMATION and $I30 FILE_NAME disagree on /test/1.txt's last "
    "access time (ntfs-lastaccess.raw)",
    "[ntfs-samples][integration][timestamps]")
{
  NtfsBrowserTests::RequireCorpusImage(kLastaccessImage);

  NtfsVolume<Strategy::NO_CACHE> const volume(
      OpenWholeDiskImage(kLastaccessImage, kSmallImagePartitionOffset));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> dir(volume);
  OpenDirPath(dir, {L"test"});
  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"1.txt");
  REQUIRE(entry.has_value());

  FILETIME indexAccess{};
  entry->GetFileTime(nullptr, nullptr, &indexAccess);

  FileRecord<Strategy::NO_CACHE> file(volume);
  file.SetAttrMask(Mask::STANDARD_INFORMATION);
  REQUIRE(file.ParseFileRecord(entry->GetFileReference()));
  REQUIRE(file.ParseAttrs());

  FILETIME stdInfoAccess{};
  file.GetFileTime(nullptr, nullptr, &stdInfoAccess);

  // ReadMe.md: 2019-03-03 12:37:55 ($STANDARD_INFORMATION) vs.
  // 2019-03-03 12:35:17 ($I30 FILE_NAME) - 2 min 38 s apart.
  CheckDeltaMatchesSeconds(TickDelta(stdInfoAccess, indexAccess),
                           kLastaccessDeltaSeconds);

  CheckRepairStreamsHoldPtrnPattern(volume);
}

TEST_CASE(
    "STANDARD_INFORMATION, FILE_NAME and $I30 FILE_NAME each carry a "
    "different creation date for /test/test.txt (ntfs-si-vs-fn.raw)",
    "[ntfs-samples][integration][timestamps]")
{
  NtfsBrowserTests::RequireCorpusImage(kSiVsFnImage);

  NtfsVolume<Strategy::NO_CACHE> const volume(
      OpenWholeDiskImage(kSiVsFnImage, kSmallImagePartitionOffset));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> dir(volume);
  OpenDirPath(dir, {L"test"});
  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"test.txt");
  REQUIRE(entry.has_value());

  FILETIME indexCreate{};
  entry->GetFileTime(nullptr, &indexCreate, nullptr);

  FileRecord<Strategy::NO_CACHE> file(volume);
  file.SetAttrMask(Mask::STANDARD_INFORMATION | Mask::FILE_NAME);
  REQUIRE(file.ParseFileRecord(entry->GetFileReference()));
  REQUIRE(file.ParseAttrs());

  FILETIME stdInfoCreate{};
  file.GetFileTime(nullptr, &stdInfoCreate, nullptr);

  // The file record's own $FILE_NAME attribute: not the parent directory's
  // $I30 copy above, but the (possibly stale) one alongside this file's own
  // $STANDARD_INFORMATION. AttrFileName<>::GetFileTime() is the same,
  // otherwise-unreachable Filename::GetFileTime() IndexEntry uses; matches
  // FileRecord::GetFileTime()'s own internal cast (src/file-record.cpp).
  const auto& fileNameAttrs = file.getAttr(AttrType::FILE_NAME);
  REQUIRE_FALSE(fileNameAttrs.empty());
  const auto* ownFileName = reinterpret_cast<
      const AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>*>(
      fileNameAttrs.front().get());
  FILETIME ownFileNameCreate{};
  ownFileName->GetFileTime(nullptr, &ownFileNameCreate, nullptr);

  // ReadMe.md: the file record's own $FILE_NAME says 2014-12-12, its
  // $STANDARD_INFORMATION says 2015-11-03, and the parent directory's $I30
  // $FILE_NAME says 2016-09-24.
  CHECK(ToDate(ownFileNameCreate) ==
        std::tuple<WORD, WORD, WORD>{2014, 12, 12});
  CHECK(ToDate(stdInfoCreate) == std::tuple<WORD, WORD, WORD>{2015, 11, 3});
  CHECK(ToDate(indexCreate) == std::tuple<WORD, WORD, WORD>{2016, 9, 24});
}

TEST_CASE(
    "STANDARD_INFORMATION and $I30 FILE_NAME disagree on two files' last "
    "access time (ntfs.raw)",
    "[ntfs-samples][integration][timestamps]")
{
  if (!std::filesystem::exists(kNtfsImage))
  {
    SKIP("ntfs.raw not present: " << kNtfsImage.string());
  }

  // This image also documents two VSS shadow copies (one hidden from
  // "vssadmin list shadows") and a stray $I30 entry surviving in $MFT record
  // slack space. Neither is checked here: this library has no VSS support,
  // and ParseFileRecord()/ParseAttrs() only ever read a record's real,
  // in-use attribute area, never its unused slack bytes.
  NtfsVolume<Strategy::NO_CACHE> volume(
      OpenWholeDiskImage(kNtfsImage, kLargeImagePartitionOffset));
  REQUIRE(volume.IsVolumeOK());

  auto const checkAccessTimeMismatch = [&volume](std::wstring_view dirName,
                                                 std::wstring_view fileName,
                                                 ULONGLONG expectedDeltaSeconds)
  {
    FileRecord<Strategy::NO_CACHE> dir(volume);
    OpenDirPath(dir, {dirName});
    const std::optional<IndexEntry> entry = dir.FindSubEntry(fileName);
    REQUIRE(entry.has_value());

    FILETIME indexAccess{};
    entry->GetFileTime(nullptr, nullptr, &indexAccess);

    FileRecord<Strategy::NO_CACHE> file(volume);
    file.SetAttrMask(Mask::STANDARD_INFORMATION);
    REQUIRE(file.ParseFileRecord(entry->GetFileReference()));
    REQUIRE(file.ParseAttrs());

    FILETIME stdInfoAccess{};
    file.GetFileTime(nullptr, nullptr, &stdInfoAccess);

    CheckDeltaMatchesSeconds(TickDelta(stdInfoAccess, indexAccess),
                             expectedDeltaSeconds);
  };

  // ReadMe.md: 2020-07-25 12:49:21 vs. 12:35:48 - 13 min 33 s apart.
  checkAccessTimeMismatch(L"test_dir_2", L"file_2_1.txt", kLongMismatchSeconds);
  // ReadMe.md: 2020-07-25 12:33:24 vs. 12:35:24 - 2 min apart.
  checkAccessTimeMismatch(L"test_dir", L"file_1.txt", kShortMismatchSeconds);
}

TEST_CASE(
    "Resolves every file record of an extremely fragmented $MFT "
    "(ntfs_extremely_fragmented_mft.raw)",
    "[ntfs-samples][integration][fragmented-mft]")
{
  const std::filesystem::path& image = NtfsBrowserTests::FragmentedMftImage();
  if (!std::filesystem::exists(image))
  {
    SKIP("ntfs_extremely_fragmented_mft.raw not present: " << image.string());
  }

  NtfsVolume<Strategy::NO_CACHE> const volume(
      OpenWholeDiskImage(image, kLargeImagePartitionOffset));
  REQUIRE(volume.IsVolumeOK());

  // ReadMe.md: these are the file records ($MFT's own base record, plus its
  // extension records) whose attributes make up $MFT's own, heavily
  // fragmented $DATA run list. Resolving each one exercises that fragmented
  // run list, through NtfsVolume::ReadMftData()/mft_extents_.
  for (const ULONGLONG recordNum :
       {0ULL, 15ULL, 16ULL, 17ULL, 18ULL, 19ULL, 20ULL, 21ULL, 22ULL,
        34799617ULL, 34799618ULL, 34799619ULL})
  {
    INFO("record " << recordNum);
    FileRecord<Strategy::NO_CACHE> record(volume);
    REQUIRE(record.ParseFileRecord(recordNum));
    CHECK(record.GetBaseRecordReference() == 0);
  }
}
