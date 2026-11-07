#pragma once

#ifndef _WIN32

  #include <string>
  #include <vector>

namespace NtfsDevice {

// Decodes each UTF-8 argument of a POSIX main() into a wide string, so the
// tools share their wide argv code with the Windows wmain().
[[nodiscard]] std::vector<std::wstring> ArgumentsAsWide(int argc, char** argv);

// Pointers to the strings of arguments, in order, ending with no null: the
// shape of a wmain() argv. The pointers stay valid while arguments does.
[[nodiscard]] std::vector<wchar_t*>
    ArgumentPointers(std::vector<std::wstring>& arguments);

}  // namespace NtfsDevice

#endif  // !_WIN32
