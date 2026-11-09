#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// How a generated stream relates to a reference file.
enum class Verdict : std::uint8_t {
  // The same bytes.
  Identical,
  // The reference file is shorter and is the start of the generated stream. A
  // probe added since the file was recorded reads after the old ones, so the
  // old file stops where the new reads begin.
  Prefix,
  // The bytes differ, or the generated stream is the shorter one.
  Different,
  // There is no reference file.
  Missing
};

// The result of one comparison.
struct Comparison {
  Verdict verdict = Verdict::Missing;
  // The first offset at which the two differ (Different only).
  std::size_t offset = 0;
  // Sizes of the reference file and of the generated stream.
  std::size_t reference_size = 0;
  std::size_t generated_size = 0;
};

// Compares generated streams with reference files.
class StreamComparator {
 public:
  // Compares generated with the file at reference.
  [[nodiscard]] static Comparison
      Compare(std::span<const BYTE> generated,
              const std::filesystem::path& reference);

  // One line that tells the verdict, with the offset or the sizes it needs.
  [[nodiscard]] static std::string Describe(const Comparison& comparison);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
