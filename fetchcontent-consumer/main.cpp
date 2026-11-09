#include <cstdlib>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/log.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>

// Exercises exported symbols of the public API, to prove the library links
// correctly when consumed via FetchContent: DLL import/export with
// BUILD_SHARED_LIBS on. The scan targets a file that does not exist and must
// end as Failed.
int main() {
  namespace Search = NTFS_BROWSER_NAMESPACE::Search;

  Search::SetLogSink({}, Search::LogLevel::Off);
  Search::Options options;
  options.target = u"fetchcontent-smoke-test-no-such-target.img";
  Search::Scan scan(options);
  return scan.Run() == Search::State::Failed ? EXIT_SUCCESS : EXIT_FAILURE;
}
