#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tuning {

// One tunable constant, by its stable name ("reader.max_reads"). The table of
// knobs is the single list that the tools' profile loader, the benchmark
// script and the documentation share: a constant that is not in it cannot be
// set from a profile.
struct Knob {
  std::string_view name;
  Family family;
  // Bit i is set when the knob serves Pattern i.
  std::uint8_t patterns;
  // True when VolumeOptions keeps one value per pattern (the reader's caps).
  bool per_pattern;
  // Bounds of a value a profile MAY set, both included.
  std::uint64_t min_value;
  std::uint64_t max_value;
  // The value the library has when nothing sets it.
  std::uint64_t default_value;
  // Reads the knob. pattern is ignored unless per_pattern.
  std::uint64_t (*get)(const VolumeOptions& options, Pattern pattern);
  // Writes the knob. The value MUST be within the bounds.
  void (*set)(VolumeOptions& options, Pattern pattern, std::uint64_t value);
};

// Every knob of families A, B and C, in a stable order.
[[nodiscard]] NTFS_BROWSER_EXPORT std::span<const Knob> Knobs();

// The knob called name, or null.
[[nodiscard]] NTFS_BROWSER_EXPORT const Knob* FindKnob(std::string_view name);

// "A", "B" or "C".
[[nodiscard]] NTFS_BROWSER_EXPORT std::string_view FamilyName(Family family);

// Sets the knob called name to value, for pattern or, when pattern is empty,
// for all the patterns it serves. Returns an empty string on success, else the
// reason: an unknown name, a value out of range, or a pattern the knob does not
// serve. Nothing changes on failure.
[[nodiscard]] NTFS_BROWSER_EXPORT std::string
    ApplyKnob(VolumeOptions& options, std::string_view name,
              std::uint64_t value,
              std::optional<Pattern> pattern = std::nullopt);

}  // namespace NTFS_BROWSER_NAMESPACE::Tuning
