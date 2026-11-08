#pragma once

// Writes the entries of a Search::Scan as the lines of a directory listing.

#include <cstdint>
#include <string>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/tool/device-access/output.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir {

// How a listing looks.
struct ListingStyle {
  // Whether a column before the name marks the state of the entry: ' ' for a
  // file that exists, '*' for a deleted one and '?' for a name whose file
  // record cannot be read. Only a scan that lists deleted entries has it.
  bool marker_column = false;
  // Whether a "Directory of" header opens the entries of each folder. It needs
  // the entries of a folder to come together, in the order of a folder walk,
  // and Entry::path.
  bool folder_headers = false;
  // Whether a line shows the full path instead of the name. It needs
  // Entry::path.
  bool full_paths = false;
  // What the volume is called in a header: the image file, the device or the
  // drive.
  std::wstring volume_label;
};

// Turns batches of entries into text. One line for each entry, then the totals.
class ListingPrinter {
 public:
  // Writes to out, which MUST outlive the printer.
  ListingPrinter(NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess::Output& out,
                 ListingStyle style);

  // Writes the lines of the entries of batch.
  void Print(const NTFS_BROWSER_NAMESPACE::Search::Batch& batch);

  // Writes the last line: the number of files and of folders printed so far.
  void PrintTotals();

 private:
  // Writes the line of one entry.
  void PrintEntry(const NTFS_BROWSER_NAMESPACE::Search::Entry& entry);

  // Opens a block with a header when entry is filed in another folder than the
  // last one.
  void PrintHeaderIfNew(const NTFS_BROWSER_NAMESPACE::Search::Entry& entry);

  NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess::Output& out_;
  ListingStyle style_;
  // The folder of the last entry, as the path up to its last '\'. Valid when
  // has_folder_ is set.
  std::u16string folder_;
  bool has_folder_ = false;
  std::uint64_t files_ = 0;
  std::uint64_t folders_ = 0;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir
