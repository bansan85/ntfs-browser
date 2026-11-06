#include <array>
#include <memory>
#include <string_view>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NtfsBrowser {

class IDiskReader;

}  // namespace NtfsBrowser

#ifdef _WIN32
  #include "io/win32-disk-reader.h"
#endif

namespace NTFS_BROWSER_NAMESPACE::Bench {

std::unique_ptr<IDiskReader> OpenPlatformReader(
    std::wstring_view path,
    const std::array<Tuning::ReaderTuning, Tuning::pattern_count>& tuning) {
#ifdef _WIN32
  auto reader = std::make_unique<Io::Win32DiskReader>(tuning);
  // A replay parses nothing, so the media kind of an image's host is no use.
  reader->SetAskHostVolume(false);
  if (!reader->Open(path)) {
    return nullptr;
  }
  return reader;
#else
  static_cast<void>(path);
  static_cast<void>(tuning);
  return nullptr;
#endif
}

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
