#include <ntfs-browser/win-types.h>

#include <stdexcept>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/hex-bytes.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The value of one hexadecimal digit. Throws std::invalid_argument otherwise.
unsigned DigitValue(char digit) {
  constexpr unsigned first_letter_value = 10;
  if (digit >= '0' && digit <= '9') {
    return static_cast<unsigned>(digit - '0');
  }
  if (digit >= 'a' && digit <= 'f') {
    return first_letter_value + static_cast<unsigned>(digit - 'a');
  }
  if (digit >= 'A' && digit <= 'F') {
    return first_letter_value + static_cast<unsigned>(digit - 'A');
  }
  throw std::invalid_argument("not a hexadecimal digit");
}

}  // namespace

std::vector<BYTE> HexBytes::Decode(std::string_view text) {
  constexpr unsigned bits_per_digit = 4;
  std::vector<BYTE> bytes;
  bool high_half = true;
  unsigned current = 0;
  for (const char digit : text) {
    if (digit == ' ') {
      continue;
    }
    const unsigned value = DigitValue(digit);
    if (high_half) {
      current = value << bits_per_digit;
    } else {
      bytes.push_back(static_cast<BYTE>(current | value));
    }
    high_half = !high_half;
  }
  if (!high_half) {
    throw std::invalid_argument("odd number of hexadecimal digits");
  }
  return bytes;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
