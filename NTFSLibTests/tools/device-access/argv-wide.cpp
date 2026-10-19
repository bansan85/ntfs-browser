#ifndef _WIN32

  #include "argv-wide.h"

  #include <cstddef>
  #include <span>
  #include <string>
  #include <string_view>
  #include <vector>

  #include <gsl/narrow>

  #include "text-convert.h"

namespace NtfsDevice {

std::vector<std::wstring> ArgumentsAsWide(int argc, char** argv) {
  const std::span<char*> raw(argv, gsl::narrow<size_t>(argc));
  std::vector<std::wstring> wide;
  wide.reserve(raw.size());
  for (const char* const argument : raw) {
    wide.push_back(Utf8ToWide(std::string_view(argument)));
  }
  return wide;
}

std::vector<wchar_t*> ArgumentPointers(std::vector<std::wstring>& arguments) {
  std::vector<wchar_t*> pointers;
  pointers.reserve(arguments.size());
  for (std::wstring& argument : arguments) {
    pointers.push_back(argument.data());
  }
  return pointers;
}

}  // namespace NtfsDevice

#endif  // !_WIN32
