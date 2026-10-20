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

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The length of a disk or volume handle, or the size of an ordinary file.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

 private:
  using HandlePtr =
      std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>;

  HandlePtr handle_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
