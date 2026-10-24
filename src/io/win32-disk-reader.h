#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <windows.h>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

// Production IDiskReader: a real disk/device, or an ordinary file treated
// the same way (CreateFileW handles both identically). Never used directly
// outside FileReader<S>::Open(), so it carries no export macro.
class Win32DiskReader : public IDiskReader {
 public:
  Win32DiskReader();
  ~Win32DiskReader() override;
  Win32DiskReader(const Win32DiskReader&) = delete;
  Win32DiskReader& operator=(const Win32DiskReader&) = delete;
  Win32DiskReader(Win32DiskReader&&) = delete;
  Win32DiskReader& operator=(Win32DiskReader&&) = delete;

  bool Open(std::wstring_view path) override;

  // Reads from addr into dest: from a range read ahead when one holds those
  // bytes, else from the disk.
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The length of a disk or volume handle, or the size of an ordinary file.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

  // Reads the announced ranges ahead, several at once, through a second,
  // asynchronous handle. The reads run while the caller works: no thread is
  // involved.
  void Prefetch(ULONGLONG offset, ULONGLONG length) const override;

  void CancelPrefetch() const override;

 private:
  using HandlePtr =
      std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>;

  class Prefetcher;

  HandlePtr handle_;
  // Null until Open() succeeds. Declared after handle_: it goes first.
  std::unique_ptr<Prefetcher> prefetcher_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
