#pragma once

#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>

#include "upcase/upcase.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// Maps the units of a name to the unit that compares equal to them. With a
// $UpCase table it folds case as NTFS does. Without one it leaves every unit as
// it is. It never uses the locale. It does not own the table: the volume that
// holds the table MUST outlive the folder.
class CaseFolder final {
 public:
  // A folder that follows table, or one that folds nothing when table is null.
  explicit CaseFolder(const UpCase::Table* table) noexcept : table_(table) {}

  // The folded unit. A unit that $UpCase does not map, a surrogate for one, is
  // returned as it is.
  [[nodiscard]] char16_t Fold(char16_t unit) const noexcept {
    return table_ != nullptr ? table_->Map(unit) : unit;
  }

  // text with every unit folded.
  [[nodiscard]] std::u16string Fold(std::u16string_view text) const {
    std::u16string folded(text);
    for (char16_t& unit : folded) {
      unit = Fold(unit);
    }
    return folded;
  }

 private:
  const UpCase::Table* table_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Search
