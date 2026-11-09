#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ntfs-browser/example/ntfsdir/arguments.h>
#include <ntfs-browser/example/ntfsdir/ntfsdir-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/tool/device-access/device-path.h>
#include <ntfs-browser/tool/device-access/output.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/tool/device-access/tool-support.h>
#include <ntfs-browser/tool/device-access/volume-target.h>

#include "listing-printer.h"
#include "log/log.h"
#include "utf16-convert.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir {

namespace {

// What the listing needs to know of the target: how to call the volume in the
// headers, and whether the scan starts in a folder or at the root.
struct TargetShape {
  std::wstring label;
  bool in_folder = false;
};

#ifdef _WIN32

// Whether character is a letter of the ASCII alphabet.
bool IsAsciiLetter(wchar_t character) {
  return (character >= L'a' && character <= L'z') ||
         (character >= L'A' && character <= L'Z');
}

#endif

// Splits target the way the scan does, to find its label and its folder.
TargetShape ShapeOf(std::wstring_view target) {
  if (const std::optional<Tool::DeviceAccess::DeviceTarget> split =
          Tool::DeviceAccess::SplitDeviceTarget(target)) {
    return {.label = Tool::DeviceAccess::WideFromPath(split->device),
            .in_folder =
                !Tool::DeviceAccess::SplitInsidePath(split->directory).empty()};
  }
#ifdef _WIN32
  if (target.size() >= 2 && IsAsciiLetter(target[0]) && target[1] == L':') {
    return {.label = std::wstring{target[0], L':'},
            .in_folder =
                !Tool::DeviceAccess::SplitInsidePath(target.substr(2)).empty()};
  }
#endif
  return {};
}

// The engine of the scan: the one that --engine names. For auto, a folder, and
// a volume that is not listed recursively, go by folder. The MftList engine
// covers the whole volume whatever --recursive says, so only a recursive
// listing of a volume reads it. The layout of the listing depends on the
// engine, so the program names it to the scan.
Search::EngineKind ResolveEngine(Search::EngineKind engine, bool in_folder,
                                 bool recursive) {
  if (engine != Search::EngineKind::Auto) {
    return engine;
  }
  return in_folder || !recursive ? Search::EngineKind::DirectoryFullCache
                                 : Search::EngineKind::MftList;
}

// Writes the percent of the scan to err when it moved to a new whole number.
void ReportProgress(const Search::Progress& progress, int& shown,
                    std::FILE* err) {
  if (!progress.percent) {
    return;
  }
  const int percent = static_cast<int>(*progress.percent);
  if (percent != shown) {
    shown = percent;
    (void)std::fputs((std::to_string(percent) + "%\n").c_str(), err);
  }
}

}  // namespace

int ListTarget(const Arguments& args, std::FILE* out, std::FILE* err) {
  Search::Options options = args.scan;
  const TargetShape shape = ShapeOf(FromUtf16(options.target));
  options.engine =
      ResolveEngine(options.engine, shape.in_folder, options.recursive);

  // The directory engines deliver the entries of a folder together, so a
  // recursive listing comes under a header for each folder. The MftList engine
  // delivers in $MFT order: each line then names its path.
  const bool folder_walk = options.engine != Search::EngineKind::MftList;
  ListingStyle style;
  style.marker_column = options.include_deleted;
  style.folder_headers = folder_walk && options.recursive;
  style.full_paths = !folder_walk;
  style.volume_label = shape.label;
  options.full_path = style.folder_headers || style.full_paths;

  Tool::DeviceAccess::Output output(Tool::DeviceAccess::output_flush_bytes,
                                    out);
  ListingPrinter printer(output, style);
  int shown_percent = -1;

  Search::Scan scan(std::move(options));
  scan.OnBatch([&](const Search::Batch& batch) {
    printer.Print(batch);
    if (args.progress) {
      ReportProgress(batch.progress, shown_percent, err);
    }
  });
  // Waits on this thread and calls the callback about every 100 ms.
  const Search::State state = scan.Run(std::chrono::milliseconds(100));
  if (state != Search::State::Finished) {
    return failure_exit_code;
  }
  printer.PrintTotals();
  return 0;
}

int RunNtfsDir(int argc, wchar_t* argv[]) {
  std::wstring error;
  const std::optional<Arguments> args = ParseArguments(argc, argv, error);
  if (!args) {
    Tool::DeviceAccess::WriteText(stderr, L"ntfsdir: " + error + L"\n\n");
    (void)std::fputs(UsageText().c_str(), stderr);
    return failure_exit_code;
  }
  if (args->common.help) {
    (void)std::fputs(UsageText().c_str(), stdout);
    return 0;
  }

  // Names print as UTF-8: a console shows them right with its code page set.
  const Tool::DeviceAccess::Utf8ConsoleOutput console;
  if (!Log::Configure(args->common.log_config)) {
    Tool::DeviceAccess::ReportLogFileError(args->common.log_config);
  }
  return ListTarget(*args, stdout, stderr);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir
