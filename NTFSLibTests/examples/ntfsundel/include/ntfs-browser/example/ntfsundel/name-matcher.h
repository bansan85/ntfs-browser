#pragma once

#include <string_view>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace UpCase {

class Table;

}  // namespace UpCase

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel {

// Compares file names and --name patterns the way the volume does: each unit
// goes through the volume's own $UpCase table, never through the locale. A
// letter of any script matches its other case. Names stay raw UTF-16 units.
class NameMatcher {
 public:
  // table MUST outlive the matcher. NtfsVolume::GetUpCaseTable() gives the
  // table of a volume.
  explicit NameMatcher(const UpCase::Table& table) noexcept;

  // Reports whether text matches pattern from end to end. '*' matches any run
  // of units, '?' matches one unit. Every other unit matches its other case.
  [[nodiscard]] bool Matches(std::wstring_view pattern,
                             std::wstring_view text) const noexcept;

  // Reports whether text starts with prefix, without regard to case.
  [[nodiscard]] bool StartsWith(std::wstring_view text,
                                std::wstring_view prefix) const noexcept;

 private:
  const UpCase::Table* table_;

  [[nodiscard]] bool SameUnit(wchar_t first, wchar_t second) const noexcept;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel
