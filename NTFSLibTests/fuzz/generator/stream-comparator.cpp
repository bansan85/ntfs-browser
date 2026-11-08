#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/stream-comparator.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

Comparison StreamComparator::Compare(std::span<const BYTE> generated,
                                     const std::filesystem::path& reference) {
  Comparison result{.generated_size = generated.size()};
  std::ifstream in(Io::ToExtendedLengthFsPath(reference), std::ios::binary);
  if (!in) {
    return result;
  }
  const std::vector<char> file((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
  result.reference_size = file.size();

  const std::size_t common = std::min(file.size(), generated.size());
  const auto mismatch = std::mismatch(
      file.begin(), file.begin() + static_cast<std::ptrdiff_t>(common),
      generated.begin(),
      [](char left, BYTE right) { return static_cast<BYTE>(left) == right; });
  result.offset = static_cast<std::size_t>(mismatch.first - file.begin());
  if (result.offset < common) {
    result.verdict = Verdict::Different;
  } else if (file.size() == generated.size()) {
    result.verdict = Verdict::Identical;
  } else if (file.size() < generated.size()) {
    result.verdict = Verdict::Prefix;
  } else {
    result.verdict = Verdict::Different;
  }
  return result;
}

std::string StreamComparator::Describe(const Comparison& comparison) {
  switch (comparison.verdict) {
    case Verdict::Identical:
      return "identical";
    case Verdict::Prefix:
      return "prefix (the file has " +
             std::to_string(comparison.reference_size) + " bytes, the stream " +
             std::to_string(comparison.generated_size) + ")";
    case Verdict::Different:
      return "different at offset " + std::to_string(comparison.offset) +
             " (the file has " + std::to_string(comparison.reference_size) +
             " bytes, the stream " + std::to_string(comparison.generated_size) +
             ")";
    case Verdict::Missing:
      return "missing";
  }
  return "unknown";
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
