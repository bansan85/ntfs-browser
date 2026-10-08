#pragma once

#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <utility>

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

// Narrow text of a native path or argv string, for a std::format argument.
[[nodiscard]] inline std::string NativeText(const std::filesystem::path& path) {
  return path.string();
}

}  // namespace NtfsCompare
