#include "recording-disk-reader.h"

#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

RecordingDiskReader::RecordingDiskReader(
    std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> inner,
    const std::filesystem::path& trace_path)
    : inner_(std::move(inner)) {
  std::filesystem::create_directories(trace_path.parent_path());
  trace_.open(trace_path, std::ios::binary | std::ios::trunc);
  if (!trace_.is_open()) {
    throw std::runtime_error("cannot create trace " + trace_path.string());
  }
}

bool RecordingDiskReader::Open(std::wstring_view path) {
  return inner_->Open(path);
}

bool RecordingDiskReader::ReadInto(LARGE_INTEGER& addr,
                                   std::span<BYTE> dest) const {
  if (!inner_->ReadInto(addr, dest)) {
    throw std::runtime_error("read at " + std::to_string(addr.QuadPart) +
                             " failed: it cannot be replayed from a trace");
  }

  trace_.write(reinterpret_cast<const char*>(dest.data()),
               gsl::narrow<std::streamsize>(dest.size()));
  if (!trace_) {
    throw std::runtime_error("cannot write the trace");
  }
  return true;
}

std::optional<ULONGLONG> RecordingDiskReader::Size() const {
  return inner_->Size();
}

void RecordingDiskReader::Prefetch(ULONGLONG offset, ULONGLONG length) const {
  inner_->Prefetch(offset, length);
}

void RecordingDiskReader::CancelPrefetch() const { inner_->CancelPrefetch(); }

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
