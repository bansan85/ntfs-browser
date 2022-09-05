#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <span>
#include <vector>

#include <windows.h>

namespace NtfsBrowser
{

class FileReader
{
 public:
  enum class Strategy
  {
    NO_CACHE,
    FULL_CACHE
  };

  FileReader(Strategy strategy);

  bool Open(std::wstring_view volume);
  std::optional<std::span<const BYTE>> Read(LARGE_INTEGER& addr,
                                            DWORD length) const;

  Strategy GetStrategy() const;

 private:
  using HandlePtr =
      std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>;

  HandlePtr handle_;
  Strategy strategy_;

  mutable std::vector<BYTE> buffer_;
  mutable std::map<size_t, std::vector<BYTE>> map_buffer_;
};

}  // namespace NtfsBrowser