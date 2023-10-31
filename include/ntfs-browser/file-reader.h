#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <span>
#include <vector>

#include <ntfs-browser/strategy.h>

#include <windows.h>

namespace NtfsBrowser
{

template <Strategy S>
class FileReader
{
 public:
  FileReader();

  bool Open(std::wstring_view volume);

  template <Strategy S2 = S>
  typename std::enable_if_t<
      std::is_same_v<std::integral_constant<Strategy, S2>,
                     std::integral_constant<Strategy, Strategy::NO_CACHE>>,
      std::optional<std::span<const BYTE>>>
      Read(LARGE_INTEGER& addr, DWORD length) const;

  template <Strategy S2 = S>
  typename std::enable_if_t<
      std::is_same_v<std::integral_constant<Strategy, S2>,
                     std::integral_constant<Strategy, Strategy::FULL_CACHE>>,
      std::optional<std::span<const BYTE>>>
      Read(LARGE_INTEGER& addr, DWORD length) const;

 private:
  using HandlePtr =
      std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>;

  HandlePtr handle_;

  mutable std::vector<BYTE> buffer_;
  mutable std::map<size_t, std::vector<BYTE>> map_buffer_;
};

}  // namespace NtfsBrowser