#pragma once

#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <utility>

#include "text-convert.h"

namespace NtfsCompare {

// Writes a std::format message to stderr.
template <class... Args>
void PrintErr(std::format_string<Args...> fmt, Args&&... args) {
  std::cerr << std::format(fmt, std::forward<Args>(args)...);
}

// Writes a std::format message to stdout.
template <class... Args>
void PrintOut(std::format_string<Args...> fmt, Args&&... args) {
  std::cout << std::format(fmt, std::forward<Args>(args)...);
}

// Narrow text of a native path or argv string, for a std::format argument. It
// is UTF-8 on every platform: the active code page of Windows would turn a
// character it lacks into '?'.
[[nodiscard]] inline std::string NativeText(const std::filesystem::path& path) {
  return NtfsDevice::WideToUtf8(NtfsDevice::WideFromPath(path));
}

}  // namespace NtfsCompare
