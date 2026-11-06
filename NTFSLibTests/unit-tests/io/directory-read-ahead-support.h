#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <ntfs-browser/io/directory-read-ahead.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "throttled-disk-reader.h"

namespace NtfsBrowser {
namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache
}  // namespace NtfsBrowser

// What the tests of the read ahead of a recursive listing share: the trees they
// list, and a run of the listing of NtfsDir over a fake volume.
namespace NTFS_BROWSER_NAMESPACE::Tests::ReadAhead {

// A tree to list, and a name for it in a failure message.
struct TreeFixture {
  std::string name;
  std::vector<BYTE> image;
};

// Many sibling directories, each with files and one subdirectory.
[[nodiscard]] std::vector<FakeTreeNode> WideTree(std::size_t directories);

// Sibling directories of files files each, none of them a directory, then
// ballast freed records that no index names: a directory, its files and its
// ballast take files + ballast + 1 records one after the other.
[[nodiscard]] std::vector<FakeTreeNode> SpacedTree(std::size_t directories,
                                                   std::size_t files,
                                                   std::size_t ballast = 0);

// Groups of directories under the root, each group holding per_group small
// directories with one file each: many small directories, with an index of the
// root that fits the record.
[[nodiscard]] std::vector<FakeTreeNode> GroupedTree(std::size_t groups,
                                                    std::size_t per_group);

// Files in each directory of BigDirectoryTree(): enough for its index to sit in
// $INDEX_ALLOCATION blocks, and few enough for the separators of the blocks to
// fit the record, which is all a fake directory holds of them.
inline constexpr std::size_t big_directory_files = 40;

// Nodes BigDirectoryTree() makes for each directory: the directory, its files,
// a subdirectory and the file in that.
inline constexpr std::size_t big_directory_nodes = big_directory_files + 3;

// Directories of big_directory_files files each.
[[nodiscard]] std::vector<FakeTreeNode>
    BigDirectoryTree(std::size_t directories);

// Sibling directories of files files each. After each file come ballast freed
// records that no index names, so the probes of one directory lie ballast + 1
// records apart.
[[nodiscard]] std::vector<FakeTreeNode> ScatteredTree(std::size_t directories,
                                                      std::size_t files,
                                                      std::size_t ballast);

// A directory tree of the given depth: each directory holds fanout
// subdirectories and one file, down to depth levels under the root. A listing
// of it grows and shrinks its stack.
[[nodiscard]] std::vector<FakeTreeNode> BranchingTree(std::size_t fanout,
                                                      std::size_t depth);

// Sibling directories of files files each, whose records have a second name in
// the same directory: a hard link for the even ones, a DOS alias for the odd
// ones. An index names such a record twice, in one block or in two.
[[nodiscard]] std::vector<FakeTreeNode> LinkedTree(std::size_t directories,
                                                   std::size_t files);

// Every tree the listing of the read ahead is compared on: wide and deep ones,
// big directories, case twins, freed directories that keep their entries, a
// directory two names lead to, a directory whose record does not parse, one
// whose index block is damaged, the orphaned blocks of the recovery fixture,
// and a chain of index blocks that runs past the depth limit of the walk.
[[nodiscard]] std::vector<TreeFixture> Fixtures();

// The four ways to open a volume that decide what a listing shows.
[[nodiscard]] std::vector<VolumeOptions> AllOptions();

// What one recursive listing of a volume did.
struct Run {
  // The listing, as NtfsDir prints it.
  std::string text;
  // What the library logged during the listing, but the lines about disk
  // reads: those follow the reads, which the read ahead moves.
  std::string log;
  // The calls the listing made on the reader, after the volume was open.
  std::vector<CountingDiskReader::Call> calls;
  int exit_code{0};
  // The conditions the listing salvaged: RecoveredCount() over it.
  std::uint64_t recovered{0};
};

// Lists the whole volume of image, recursively, with the options and the read
// ahead of dirs directories (0 is off) and bytes bytes at most. The reader is
// in memory, and says it is a device of the given kind.
template <Cache::Strategy S>
[[nodiscard]] Run
    ListRecursively(const std::vector<BYTE>& image, VolumeOptions options,
                    std::size_t dirs, std::uint64_t bytes,
                    Tuning::MediaKind kind = Tuning::MediaKind::Unknown);

// While it lives, every refill of the standing lane works its zone out again
// and compares it with the one it kept. At the end, it checks that none
// differed.
class VerifiedLaneZone {
 public:
  VerifiedLaneZone() noexcept;
  ~VerifiedLaneZone();
  VerifiedLaneZone(const VerifiedLaneZone&) = delete;
  VerifiedLaneZone& operator=(const VerifiedLaneZone&) = delete;
  VerifiedLaneZone(VerifiedLaneZone&&) = delete;
  VerifiedLaneZone& operator=(VerifiedLaneZone&&) = delete;

 private:
  std::size_t mismatches_before_;
};

// What the read ahead did to the reader during one recursive listing.
struct LaneRun {
  // The standing lane's calls, in order, the withdrawals made when the read
  // ahead was destroyed included.
  std::vector<CountingDiskReader::Call> lane;
  // Every other call the listing made on the reader.
  std::vector<CountingDiskReader::Call> others;
  // Read just before the read ahead was destroyed.
  Io::DirectoryReadAheadStats stats;
};

// Lists the whole volume of image recursively, as ListTree() of NtfsDir does:
// the same calls on DirectoryReadAhead, in the same order, with the options as
// they are. The reader is in memory, and says it is a device of the given
// kind. With raw_callback, the volume has a raw attribute callback, which
// turns the lane off.
template <Cache::Strategy S>
[[nodiscard]] LaneRun
    ListForLane(const std::vector<BYTE>& image, const VolumeOptions& options,
                Tuning::MediaKind kind = Tuning::MediaKind::Unknown,
                bool raw_callback = false);

// The same with the walk tuning of options left as it is: what NtfsDir does
// when it is given no tuning.
template <Cache::Strategy S>
[[nodiscard]] Run ListWithDefaultTuning(const std::vector<BYTE>& image,
                                        const VolumeOptions& options);

// The same, over a simulated disk of model: the time the listing took on its
// clock, in nanoseconds, with what the disk did.
struct TimedRun {
  Run run;
  std::uint64_t elapsed_ns{0};
  ThrottleStats disk;
};

template <Cache::Strategy S>
[[nodiscard]] TimedRun
    ListOnSimulatedDisk(const std::vector<BYTE>& image, VolumeOptions options,
                        std::size_t dirs, std::uint64_t bytes,
                        const DiskModel& model,
                        Tuning::MediaKind kind = Tuning::MediaKind::Unknown);

// A hard disk: a long seek, one lane, as NtfsBench simulates it.
[[nodiscard]] DiskModel HardDiskModel();

// The first line where left and right differ, with the lines before it, to
// read in a failure message. Empty when they are equal.
[[nodiscard]] std::string FirstDifference(const std::string& left,
                                          const std::string& right);

// Where the $MFT of an image lies, to tell the sectors a run of records may
// read across.
struct MftSpan {
  ULONGLONG first{0};
  ULONGLONG end{0};
};

[[nodiscard]] MftSpan MftSpanOf(const std::vector<BYTE>& image);

// Sectors of 512 bytes the reads of calls touched, and how many times each.
// image_bytes is the size of the image they were made on.
[[nodiscard]] std::vector<std::uint32_t>
    SectorReadCounts(const std::vector<CountingDiskReader::Call>& calls,
                     std::size_t image_bytes);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::ReadAhead
