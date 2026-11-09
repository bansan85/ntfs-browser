#include <cstdlib>
#include <string_view>

// Only the public headers: the target of this file never gets the src/ folder
// of the library, so an include of an internal header fails the build.
#include <ntfs-browser/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader-types.h>
#include <ntfs-browser/reader.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/types.h>

// Scans a target that does not exist: the scan ends as Failed, and the log line
// that says why goes to the sink, never to the console.
int main() {
  namespace Lib = NTFS_BROWSER_NAMESPACE;

  int lines = 0;
  Lib::SetLogSink([&lines](Lib::LogLevel, std::string_view) { ++lines; },
                  Lib::LogLevel::Trace);

  Lib::Options options;
  options.target = u"ntfs-browser-public-api-no-such-target.img";
  Lib::Scan scan(options);
  if (scan.Run() != Lib::State::Failed) {
    return EXIT_FAILURE;
  }

  Lib::SetLogSink({}, Lib::LogLevel::Off);
  return lines > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
