#include "listing-printer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/local-time.h>
#include <ntfs-browser/tool/device-access/output.h>
#include <ntfs-browser/types.h>

#include "utf16-convert.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir {

namespace {

// Separates the names of a path that Entry::path holds.
constexpr char16_t path_separator = u'\\';

// Width of the date and time column, "2024-01-31  23:59", kept blank when the
// platform cannot show the time.
constexpr std::size_t time_column_width = 17;

// The marker column of an entry in a listing that shows deleted entries.
char MarkerOf(NTFS_BROWSER_NAMESPACE::Standing standing) {
  switch (standing) {
    case NTFS_BROWSER_NAMESPACE::Standing::Live:
      return ' ';
    case NTFS_BROWSER_NAMESPACE::Standing::Unreadable:
      return '?';
    case NTFS_BROWSER_NAMESPACE::Standing::Deleted:
    case NTFS_BROWSER_NAMESPACE::Standing::Reused:
      break;
  }
  return '*';
}

// The folder that path is filed in: the path up to its last separator. Empty
// for the root.
std::u16string_view FolderOf(std::u16string_view path) {
  const std::size_t last = path.rfind(path_separator);
  return last == std::u16string_view::npos ? std::u16string_view()
                                           : path.substr(0, last);
}

}  // namespace

ListingPrinter::ListingPrinter(Tool::DeviceAccess::Output& out,
                               ListingStyle style)
    : out_(out), style_(std::move(style)) {}

void ListingPrinter::Print(const NTFS_BROWSER_NAMESPACE::Batch& batch) {
  for (const NTFS_BROWSER_NAMESPACE::Entry& entry : batch.entries) {
    if (style_.folder_headers) {
      PrintHeaderIfNew(entry);
    }
    PrintEntry(entry);
  }
}

void ListingPrinter::PrintHeaderIfNew(
    const NTFS_BROWSER_NAMESPACE::Entry& entry) {
  const std::u16string_view folder = FolderOf(entry.path);
  if (has_folder_ && folder_ == folder) {
    return;
  }
  has_folder_ = true;
  folder_ = folder;
  out_.EndLine();
  out_.Append("Directory of ");
  out_.AppendText(style_.volume_label);
  out_.AppendText(FromUtf16(folder));
  out_.EndLine();
}

void ListingPrinter::PrintEntry(const NTFS_BROWSER_NAMESPACE::Entry& entry) {
  const std::optional<Tool::DeviceAccess::LocalTime> time =
      Tool::DeviceAccess::UtcTicksToLocalTime(entry.modified.ticks);
  if (time) {
    out_.AppendNumber(time->year, 0);
    out_.Append("-");
    out_.AppendNumber(time->month, 2, '0');
    out_.Append("-");
    out_.AppendNumber(time->day, 2, '0');
    out_.Append("  ");
    out_.AppendNumber(time->hour, 2, '0');
    out_.Append(":");
    out_.AppendNumber(time->minute, 2, '0');
  } else {
    out_.Append(std::string(time_column_width, ' '));
  }
  out_.Append(entry.is_directory ? "\t<DIR>    " : "\t         ");
  if (entry.is_directory) {
    folders_++;
  } else {
    files_++;
    out_.AppendNumber(entry.size, 0);
  }

  out_.Append("\t<");
  out_.Append(
      (entry.attributes & NTFS_BROWSER_NAMESPACE::Attribute::read_only) != 0U
          ? "R"
          : " ");
  out_.Append((entry.attributes & NTFS_BROWSER_NAMESPACE::Attribute::hidden) !=
                      0U
                  ? "H"
                  : " ");
  out_.Append((entry.attributes & NTFS_BROWSER_NAMESPACE::Attribute::system) !=
                      0U
                  ? "S"
                  : " ");
  out_.Append(">\t");
  if (style_.marker_column) {
    out_.Append(std::string(1, MarkerOf(entry.standing)));
    out_.Append(" ");
  }
  out_.AppendText(FromUtf16(style_.full_paths ? entry.path : entry.name));
  out_.EndLine();
}

void ListingPrinter::PrintTotals() {
  out_.PrintLine("Files: {}, Directories: {}", files_, folders_);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir
