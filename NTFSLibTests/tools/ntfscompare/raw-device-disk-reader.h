#pragma once

#ifndef _WIN32

  #include <ntfs-browser/win-types.h>

  #include <string_view>

  #include <ntfs-browser/disk-reader.h>

namespace NtfsCompare {

// Production IDiskReader for Linux: opens a raw block device node (eg.
// "/dev/sdb1") read-only and reads it with pread(). A partition's own device
// node already exposes offsets relative to the partition's start, so unlike
// a whole-disk image file, no separate offsetting is needed.
class RawDeviceDiskReader : public NtfsBrowser::IDiskReader {
 public:
  RawDeviceDiskReader() = default;
  RawDeviceDiskReader(const RawDeviceDiskReader&) = delete;
  RawDeviceDiskReader& operator=(const RawDeviceDiskReader&) = delete;
  RawDeviceDiskReader(RawDeviceDiskReader&&) = delete;
  RawDeviceDiskReader& operator=(RawDeviceDiskReader&&) = delete;
  ~RawDeviceDiskReader() override;

  bool Open(std::wstring_view path) override;
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  int fd_ = -1;
};

}  // namespace NtfsCompare

#endif  // !_WIN32
