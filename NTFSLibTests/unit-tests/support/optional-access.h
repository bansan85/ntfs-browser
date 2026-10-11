#pragma once

#include <optional>
#include <utility>

namespace NtfsBrowserTests {

// Reads an optional a test has already checked. Throws when it is empty:
// Catch2 reports that as a failed test, and clang-tidy's
// bugprone-unchecked-optional-access does not understand a REQUIRE.
template <typename T>
[[nodiscard]] T& Unwrap(std::optional<T>& opt) {
  if (!opt.has_value()) {
    throw std::bad_optional_access();
  }
  return *opt;
}

template <typename T>
[[nodiscard]] const T& Unwrap(const std::optional<T>& opt) {
  if (!opt.has_value()) {
    throw std::bad_optional_access();
  }
  return *opt;
}

template <typename T>
[[nodiscard]] T Unwrap(std::optional<T>&& opt) {
  if (!opt.has_value()) {
    throw std::bad_optional_access();
  }
  return *std::move(opt);
}

}  // namespace NtfsBrowserTests
