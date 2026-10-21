#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Everything a test learns from one scan of a standalone $MFT, besides the
// tree itself.
struct StandaloneMftScan {
  Mft::MftTree tree;
  // Bytes the cache holds once the scan is over. 0 for NoCache.
  std::uint64_t held_bytes{0};
  // Size of the largest read the disk reader received.
  std::uint64_t largest_read{0};
  // The sizes the reader took from the file.
  DWORD record_size{0};
  DWORD cluster_size{0};
};

// A scratch file, written at construction and removed on scope exit. Its name
// is unique to the process and to the file: the test processes of a parallel
// ctest run share one temporary folder.
class TempFile {
 public:
  explicit TempFile(std::span<const BYTE> bytes);
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  ~TempFile();

  [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// Scans file as the $MFT of a volume (StandaloneMftDiskReader), with the given
// volume options and cache strategy. repair_extent is
// StandaloneMftDiskReader::SetRepairDeclaredExtent(). Fails the running test
// when the file does not open as a $MFT or the volume does not open.
[[nodiscard]] StandaloneMftScan ScanStandaloneMft(
    const std::filesystem::path& file, Cache::Strategy strategy,
    const VolumeOptions& options = {}, bool repair_extent = false);

// Opens file as the $MFT of a NoCache volume, for a test that reads records
// itself. Fails the running test when it does not open.
[[nodiscard]] std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>>
    OpenStandaloneMftVolume(const std::filesystem::path& file,
                            const VolumeOptions& options = {},
                            bool repair_extent = false);

// Same options as ScanStandaloneMft(), spelled for the two modes a test runs
// every fixture in. include_deleted is on in both, so the damaged counts of
// the two modes cover the same records.
[[nodiscard]] VolumeOptions StrictOptions(bool include_deleted = true);
[[nodiscard]] VolumeOptions RecoveringOptions(bool include_deleted = true);

// A FILETIME as whole seconds since 1601-01-01 UTC. Entry times are local
// time on Windows, and UTC elsewhere: this undoes the Windows conversion.
[[nodiscard]] std::uint64_t EntryUtcSeconds(const FILETIME& local_time);

// Same, in 100 ns ticks.
[[nodiscard]] std::uint64_t EntryUtcTicks(const FILETIME& local_time);

// Ticks since 1601-01-01 UTC of a calendar instant, with microseconds.
[[nodiscard]] std::uint64_t UtcTicks(int year, unsigned month, unsigned day,
                                     unsigned hour, unsigned minute,
                                     unsigned second, unsigned microsecond = 0);

// One line of a Sleuth Kit `fls -r -p -l` listing, as the dfir_ntfs oracles
// hold them.
struct FlsLine {
  // "r/r" (file), "d/d" (directory) or "V/V" (the virtual $OrphanFiles).
  std::string kind;
  ULONGLONG record{0};
  // The `*` of Sleuth Kit marks a deleted file. The kept listings hold none.
  bool deleted{false};
  unsigned attribute_type{0};
  // Path from the root, UTF-8, without a leading separator and without a
  // stream name.
  std::string path;
  // The stream name after the last ':' of the name field. Empty for the
  // unnamed stream.
  std::string stream;
  // Modification, access, MFT change and creation time, as whole seconds
  // since 1601-01-01 UTC. The listing's own time zone is already undone.
  std::array<std::uint64_t, 4> times{};
  ULONGLONG size{0};
};

// Parses a listing. LF and CRLF line ends are both accepted. A line that does
// not have nine tab-separated fields, or whose time zone is neither UTC nor
// MSK, fails the running test.
[[nodiscard]] std::vector<FlsLine>
    ParseFlsListing(const std::filesystem::path& file);

// A path of MftTree::GetPath() as a listing writes it: UTF-8, separated by
// '/', without the leading separator.
[[nodiscard]] std::string ListingPath(const std::wstring& tree_path);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
