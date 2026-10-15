#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "corpus-test-support.h"
#include "optional-access.h"

using NTFS_BROWSER_NAMESPACE::Attr::AttrBase;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

// The "ntfs-samples" forensic test image corpus, documented in that repo's
// ReadMe.md. Each image is checked in there as a gzip/rar archive, too large
// to check into this one too.
const std::filesystem::path& samples_dir =
    NTFS_BROWSER_NAMESPACE::Tests::NtfsSamplesDir();

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

// Parses dir's own file record as the volume's root directory, ready for
// FindSubEntry(). dir must already be constructed on that volume.
inline void OpenRootDir(
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>& dir) {
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(dir.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(dir.ParseAttrs());
}

// Looks up name under dir's current directory and reparses dir in place as
// that subdirectory.
inline void OpenSubDir(
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>& dir,
    std::wstring_view name) {
  const std::optional<IndexEntry> entry = dir.FindSubEntry(name);
  REQUIRE(entry.has_value());
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(dir.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileReference()));
  REQUIRE(dir.ParseAttrs());
}

// Navigates dir down a directory path, one component at a time, from the
// volume root.
inline void OpenDirPath(
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>& dir,
    std::initializer_list<std::wstring_view> parts) {
  OpenRootDir(dir);
  for (const std::wstring_view part : parts) {
    OpenSubDir(dir, part);
  }
}

// True if data is entirely the repeating 4-byte "PTRN" pattern these corpus
// images were filled with before formatting, starting at whatever phase of
// the pattern data[0] happens to land on.
inline bool MatchesPtrnPattern(std::span<const BYTE> data) {
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
inline bool ContainsPtrnRun(std::span<const BYTE> data, size_t min_run_length) {
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
inline void CheckRepairStreamsHoldPtrnPattern(
    const NtfsVolume<Cache::Strategy::NoCache>& volume) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> dir(volume);
  OpenDirPath(dir, {L"$Extend", L"$RmMetadata"});

  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"$Repair");
  REQUIRE(entry.has_value());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> file(volume);
  file.SetAttrMask(Attr::Mask::Data);
  REQUIRE(file.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileReference()));
  REQUIRE(file.ParseAttrs());

  for (const std::wstring_view stream_name : {L"$Corrupt", L"$Verify"}) {
    const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<Cache::Strategy::NoCache>*
        stream = file.FindStream(stream_name);
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
inline ULONGLONG TickDelta(const FILETIME& first, const FILETIME& second) {
  const ULONGLONG first_ticks =
      NTFS_BROWSER_NAMESPACE::Tests::FileTimeToTicks(first);
  const ULONGLONG second_ticks =
      NTFS_BROWSER_NAMESPACE::Tests::FileTimeToTicks(second);
  return first_ticks > second_ticks ? first_ticks - second_ticks
                                    : second_ticks - first_ticks;
}

// ReadMe.md documents each timestamp only to the whole second, so each one's
// true sub-second remainder is unknown; the true delta can therefore land
// anywhere in the open, one-second-wide margin around expectedSeconds.
inline void CheckDeltaMatchesSeconds(ULONGLONG delta_ticks,
                                     ULONGLONG expected_seconds) {
  CHECK(delta_ticks > (expected_seconds - 1) * ticks_per_second);
  CHECK(delta_ticks < (expected_seconds + 1) * ticks_per_second);
}

// Year/month/day of ft, converted from its (local-time) FILETIME.
inline std::tuple<WORD, WORD, WORD> ToDate(const FILETIME& file_time) {
  return NTFS_BROWSER_NAMESPACE::Tests::FileTimeToDate(file_time);
}

}  // namespace
