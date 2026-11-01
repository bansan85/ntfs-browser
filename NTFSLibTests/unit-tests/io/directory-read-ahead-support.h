#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "throttled-disk-reader.h"

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
};

// Lists the whole volume of image, recursively, with the options and the read
// ahead of dirs directories (0 is off) and bytes bytes at most. The reader is
// in memory.
template <Cache::Strategy S>
[[nodiscard]] Run ListRecursively(const std::vector<BYTE>& image,
                                  VolumeOptions options, std::size_t dirs,
                                  std::uint64_t bytes);

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
                        const DiskModel& model);

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
