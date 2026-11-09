#include <cstdlib>
#include <string_view>

#include <ntfs-browser/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/types.h>

// Exercises exported symbols of the public API, to prove the library links
// correctly when consumed via FetchContent: DLL import/export with
// BUILD_SHARED_LIBS on. The scan targets a file that does not exist and must
// end as Failed.
int main() {
  namespace Lib = NTFS_BROWSER_NAMESPACE;

  Lib::SetLogSink({}, Lib::LogLevel::Off);
  Lib::Options options;
  options.target = u"fetchcontent-smoke-test-no-such-target.img";
  Lib::Scan scan(options);
  return scan.Run() == Lib::State::Failed ? EXIT_SUCCESS : EXIT_FAILURE;
}
