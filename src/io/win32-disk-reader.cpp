#include "io/win32-disk-reader.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <gsl/narrow>
#include <winioctl.h>

#include <ntfs-browser/namespace.h>

#include "bench/recorder.h"
#include "io/extended-path.h"
#include "io/host-media-kind.h"
#include "io/win32-prefetcher.h"
#include "log/ntfs-common.h"
#include "tuning/tuning.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// True for the handle of a file, false for a volume or a disk: those answer
// for themselves, and a volume has no file information.
bool IsPlainFile(HANDLE handle) noexcept {
  BY_HANDLE_FILE_INFORMATION info{};
  return GetFileType(handle) == FILE_TYPE_DISK &&
         GetFileInformationByHandle(handle, &info) != FALSE &&
         (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

Win32DiskReader::Win32DiskReader(const ReaderCaps& caps)
    : handle_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle)), caps_(caps) {}

Win32DiskReader::~Win32DiskReader() = default;

bool Win32DiskReader::Open(std::wstring_view path) {
  // What was read ahead belongs to the previous store.
  prefetcher_.reset();
  media_kind_ = Tuning::MediaKind::Unknown;

  // Extended, so a path of more than 260 units opens.
  std::wstring path_z = ToExtendedLengthPath(path);
  // SEQUENTIAL_SCAN is only a hint: it asks the cache manager for more
  // read-ahead and earlier eviction of pages already read, so an $MFT scan
  // does not flood the system cache. A random read stays correct.
  handle_ = HandlePtr(
      CreateFileW(path_z.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_READONLY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr),
      &CloseHandle);
  if (handle_.get() == INVALID_HANDLE_VALUE) {
    return false;
  }

  media_kind_ = SeekPenaltyKindOf(handle_.get());
  // A plain file refuses the query: its volume answers for it.
  if (ask_host_volume_ && media_kind_ == Tuning::MediaKind::Unknown &&
      IsPlainFile(handle_.get())) {
    media_kind_ = QueryHostMediaKind(path_z);
  }
  prefetcher_ = std::make_unique<Win32Prefetcher>(std::move(path_z), caps_);
  return true;
}

Tuning::MediaKind Win32DiskReader::GetMediaKind() const { return media_kind_; }

bool Win32DiskReader::ReadInto(LARGE_INTEGER& addr,
                               std::span<BYTE> dest) const {
  // OVERLAPPED carries the offset as unsigned: a negative one would wrap.
  if (addr.QuadPart < 0) {
    Log::Error("Cannot read file at adress {}: negative address",
               addr.QuadPart);
    return false;
  }

  if (prefetcher_ != nullptr &&
      prefetcher_->Serve(static_cast<ULONGLONG>(addr.QuadPart), dest)) {
    return true;
  }

  if (!ReadSynchronously(addr, dest)) {
    Log::Error("Cannot read file at adress {}", addr.QuadPart);
    return false;
  }
  return true;
}

bool Win32DiskReader::ReadSynchronously(const LARGE_INTEGER& addr,
                                        std::span<BYTE> dest) const {
  // The handle is synchronous, so OVERLAPPED only carries the offset. ReadFile
  // reads there and ignores the handle's own file pointer.
  OVERLAPPED overlapped{};
  overlapped.Offset = addr.LowPart;
  overlapped.OffsetHigh = gsl::narrow<DWORD>(addr.HighPart);

  const bool bench = Bench::Detail::SessionOn();
  const std::uint64_t began = bench ? Bench::Detail::NowNs() : 0;
  DWORD len = 0;
  const bool read =
      ReadFile(handle_.get(), dest.data(), gsl::narrow<DWORD>(dest.size()),
               &len, &overlapped) != FALSE &&
      len == dest.size();
  if (bench) {
    // The caller waited for the whole read: nothing announced it.
    const std::uint32_t in_flight =
        prefetcher_ != nullptr ? prefetcher_->InFlightReads() + 1 : 1;
    Bench::Detail::NoteDiskRead(
        static_cast<std::uint64_t>(addr.QuadPart), dest.size(),
        Bench::Detail::DiskReadOrigin::Synchronous, in_flight);
    Bench::Detail::NoteRead(Bench::ReadClass::Plan,
                            Bench::Detail::NowNs() - began, in_flight);
  }
  return read;
}

std::optional<ULONGLONG> Win32DiskReader::Size() const {
  // Answers for a disk or a volume handle, and fails for an ordinary file.
  GET_LENGTH_INFORMATION length_info{};
  DWORD returned = 0;
  if (DeviceIoControl(handle_.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                      &length_info, sizeof(length_info), &returned,
                      nullptr) != FALSE &&
      length_info.Length.QuadPart >= 0) {
    return gsl::narrow<ULONGLONG>(length_info.Length.QuadPart);
  }

  LARGE_INTEGER file_size{};
  if (GetFileSizeEx(handle_.get(), &file_size) != FALSE &&
      file_size.QuadPart >= 0) {
    return gsl::narrow<ULONGLONG>(file_size.QuadPart);
  }
  return std::nullopt;
}

void Win32DiskReader::Prefetch(ULONGLONG offset, ULONGLONG length) const {
  if (prefetcher_ != nullptr) {
    prefetcher_->Announce(offset, length);
  }
}

void Win32DiskReader::CancelPrefetch() const {
  if (prefetcher_ != nullptr) {
    prefetcher_->Cancel();
  }
}

void Win32DiskReader::PrefetchStanding(ULONGLONG offset,
                                       ULONGLONG length) const {
  if (prefetcher_ != nullptr) {
    prefetcher_->AnnounceStanding(offset, length);
  }
}

void Win32DiskReader::CancelStanding(ULONGLONG offset, ULONGLONG length) const {
  if (prefetcher_ != nullptr) {
    prefetcher_->CancelStanding(offset, length);
  }
}

void Win32DiskReader::SetReadPattern(Tuning::Pattern pattern) const {
  if (prefetcher_ != nullptr) {
    prefetcher_->SetPattern(pattern);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
