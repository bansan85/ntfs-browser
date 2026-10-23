#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <string>
#include <vector>

#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

#include "external-mft-support.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Names the first field two entries disagree on. Empty when they are equal.
// strict_left is true when left comes from a strict scan and right from a
// recovering one: a strict scan drops the damaged parent of a name, so the
// name may be linked to its parent only on the right.
[[nodiscard]] std::string FirstDifference(const Mft::MftTree::Entry& left,
                                          const Mft::MftTree::Entry& right,
                                          bool strict_left = false);

// Names the first thing two scans disagree on. Empty when they are the same
// kept records with the same counters.
[[nodiscard]] std::string FirstDifference(const Mft::MftTree& left,
                                          const Mft::MftTree& right);

// What every field of a listing line says, checked against a tree. Collects
// the first few differences, since a listing holds thousands of lines.
struct ListingReport {
  std::size_t checked{0};
  std::vector<std::string> differences;

  void Note(const std::string& message);
};

// Compares every line of a Sleuth Kit listing with the tree. The tree holds
// the files in use. Conventions of the listing:
//   - the `V/V` line is the virtual $OrphanFiles directory, in no record;
//   - times are the four of $STANDARD_INFORMATION, in the order modification,
//     access, MFT change, creation, with a whole-second precision;
//   - the size of an `r/r` line of the unnamed $DATA is the stream's real
//     size, and the size of a `d/d` line is its index's, so it is not compared;
//   - a named stream is a line of its own (`file:stream`), and a directory
//     with only named streams has only those lines;
//   - a name that is only a DOS 8.3 alias is never listed;
//   - the root directory is never listed;
//   - the `*` of a deleted file is not in any of the kept listings.
[[nodiscard]] ListingReport
    CompareWithListing(const Mft::MftTree& tree,
                       const std::vector<FlsLine>& lines);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
