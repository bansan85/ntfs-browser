#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include <sys/types.h>

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/sparse-capture.h>

#ifdef _WIN32
  #include <windows.h>
  #include <winioctl.h>
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

namespace {

// A zero piece is not written: the unit of the test is a 4096-byte sector, the
// largest NTFS uses, so a hole stays aligned to what a disk reads.
constexpr std::size_t piece_bytes = 4096;

// Whether every byte of bytes is zero.
bool IsZero(std::span<const BYTE> bytes) {
  return std::ranges::all_of(bytes, [](BYTE b) { return b == 0; });
}

}  // namespace

class SparseCapture::Impl {
 public:
  explicit Impl(const std::filesystem::path& path) {
#ifdef _WIN32
    handle_ = CreateFileW(
        NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(path).c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      DWORD returned = 0;
      DeviceIoControl(handle_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                      &returned, nullptr);
    }
#else
    descriptor_ = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
#endif
  }

  ~Impl() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
#else
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
#endif
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] bool Ok() const noexcept {
#ifdef _WIN32
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return descriptor_ >= 0;
#endif
  }

  // Writes bytes at offset. False on failure.
  bool WriteAt(std::uint64_t offset, std::span<const BYTE> bytes) {
#ifdef _WIN32
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    DWORD written = 0;
    return WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()),
                     &written, &overlapped) != FALSE &&
           written == bytes.size();
#else
    return pwrite(descriptor_, bytes.data(), bytes.size(),
                  static_cast<off_t>(offset)) ==
           static_cast<ssize_t>(bytes.size());
#endif
  }

  bool Grow(std::uint64_t bytes) {
#ifdef _WIN32
    LARGE_INTEGER current{};
    if (GetFileSizeEx(handle_, &current) == FALSE) {
      return false;
    }
    if (static_cast<std::uint64_t>(current.QuadPart) >= bytes) {
      return true;
    }
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(bytes);
    return SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN) != FALSE &&
           SetEndOfFile(handle_) != FALSE;
#else
    struct stat info{};
    if (fstat(descriptor_, &info) != 0) {
      return false;
    }
    return static_cast<std::uint64_t>(info.st_size) >= bytes ||
           ftruncate(descriptor_, static_cast<off_t>(bytes)) == 0;
#endif
  }

 private:
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int descriptor_{-1};
#endif
};

SparseCapture::SparseCapture(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>(path)) {
  if (!impl_->Ok()) {
    error_ = "cannot create " + path.string();
  }
}

SparseCapture::~SparseCapture() = default;

bool SparseCapture::Ok() const noexcept {
  return impl_->Ok() && error_.empty();
}

void SparseCapture::Write(std::uint64_t offset, std::span<const BYTE> bytes) {
  if (!impl_->Ok() || !error_.empty()) {
    return;
  }
  // The pieces that hold data, merged when they touch.
  std::size_t start = 0;
  std::size_t piece = 0;
  const auto flush = [&](std::size_t end) {
    if (end > start &&
        !impl_->WriteAt(offset + start, bytes.subspan(start, end - start))) {
      error_ = "cannot write the capture file";
    }
  };
  for (; piece < bytes.size(); piece += piece_bytes) {
    const std::size_t length = (std::min)(piece_bytes, bytes.size() - piece);
    if (IsZero(bytes.subspan(piece, length))) {
      flush(piece);
      start = piece + length;
    }
  }
  flush(bytes.size());
}

bool SparseCapture::SetSize(std::uint64_t bytes) { return impl_->Grow(bytes); }

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
