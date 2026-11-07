#include <ntfs-browser/win-types.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>

namespace NTFS_BROWSER_NAMESPACE::Test::DiskReaders {

MemoryDiskReader::MemoryDiskReader(std::vector<BYTE> data)
    : data_(std::move(data)) {}

bool MemoryDiskReader::Open(std::wstring_view path) {
  std::ifstream input(std::filesystem::path(path), std::ios::binary);
  if (!input) {
    return false;
  }

  data_.assign(std::istreambuf_iterator<char>(input),
               std::istreambuf_iterator<char>());
  return true;
}

bool MemoryDiskReader::ReadInto(LARGE_INTEGER& addr,
                                std::span<BYTE> dest) const {
  if (addr.QuadPart < 0 ||
      static_cast<ULONGLONG>(addr.QuadPart) + dest.size() > data_.size()) {
    return false;
  }

  // The check above bounds addr + dest.size() by data_.size().
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  std::memcpy(dest.data(), &data_[static_cast<size_t>(addr.QuadPart)],
              dest.size());
  return true;
}

std::optional<ULONGLONG> MemoryDiskReader::Size() const { return data_.size(); }

NTFS_BROWSER_NAMESPACE::Tuning::MediaKind
    MemoryDiskReader::GetMediaKind() const {
  return kind_;
}

void MemoryDiskReader::SetMediaKind(
    NTFS_BROWSER_NAMESPACE::Tuning::MediaKind kind) noexcept {
  kind_ = kind;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::DiskReaders
