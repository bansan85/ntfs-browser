#include <array>
#include <memory>
#include <string_view>

#include <ntfs-browser/bench/platform-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Bench {

std::unique_ptr<IDiskReader> OpenPlatformReader(
    std::wstring_view path,
    const std::array<Tuning::ReaderTuning, Tuning::pattern_count>& tuning) {
  static_cast<void>(path);
  static_cast<void>(tuning);
  return nullptr;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
