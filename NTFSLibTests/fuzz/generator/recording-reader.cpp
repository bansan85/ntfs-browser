#include <ntfs-browser/win-types.h>

#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/recording-reader.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

RecordingReader::RecordingReader(
    std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> inner,
    std::vector<BYTE>& sink)
    : inner_(std::move(inner)), sink_(&sink) {}

bool RecordingReader::Open(std::wstring_view path) {
  return inner_->Open(path);
}

bool RecordingReader::ReadInto(LARGE_INTEGER& addr,
                               std::span<BYTE> dest) const {
  if (!inner_->ReadInto(addr, dest)) {
    throw std::runtime_error("read at " + std::to_string(addr.QuadPart) +
                             " failed: it cannot be replayed from a stream");
  }
  const std::scoped_lock lock(mutex_);
  sink_->insert(sink_->end(), dest.begin(), dest.end());
  return true;
}

std::optional<ULONGLONG> RecordingReader::Size() const {
  return inner_->Size();
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
