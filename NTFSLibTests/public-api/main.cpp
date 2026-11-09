#include <cstdlib>
#include <string_view>

// Only the public headers: the target of this file never gets the src/ folder
// of the library, so an include of an internal header fails the build.
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/log.h>
#include <ntfs-browser/search/reader-types.h>
#include <ntfs-browser/search/reader.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>

// Scans a target that does not exist: the scan ends as Failed, and the log line
// that says why goes to the sink, never to the console.
int main() {
  namespace Search = NTFS_BROWSER_NAMESPACE::Search;

  int lines = 0;
  Search::SetLogSink([&lines](Search::LogLevel, std::string_view) { ++lines; },
                     Search::LogLevel::Trace);

  Search::Options options;
  options.target = u"ntfs-browser-public-api-no-such-target.img";
  Search::Scan scan(options);
  if (scan.Run() != Search::State::Failed) {
    return EXIT_FAILURE;
  }

  Search::SetLogSink({}, Search::LogLevel::Off);
  return lines > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
