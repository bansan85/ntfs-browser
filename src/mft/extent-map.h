#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/cache/strategy.h>

namespace NtfsBrowser::Mft {

// Where $MFT's own unnamed DATA attribute lives on disk: the base extent at
// VCN 0, and the continuations that map the rest. Records past the base record
// are read through this map.
template <Cache::Strategy S>
class ExtentMap {
 public:
  // cluster_size and file_record_size come from the boot sector. The map does
  // not keep a link to the volume.
  ExtentMap(DWORD cluster_size, DWORD file_record_size) noexcept;

  // Sets the base extent and accepts it. Its VCN range is not checked against
  // an $ATTRIBUTE_LIST entry.
  void SetBase(const Attr::AttrBase<S>& base);

  // True once SetBase() has run.
  [[nodiscard]] bool HasBase() const noexcept;

  // Accepts a continuation whose start VCN matches expected_start_vcn, unless
  // it is named, inverted, too large or overlapping. A rejected one is dropped
  // with a warning.
  void TryAdd(const Attr::AttrBase<S>& attr, ULONGLONG expected_start_vcn);

  // True if [byte_offset, byte_offset + length) is fully mapped. No I/O.
  [[nodiscard]] bool IsRangeMapped(ULONGLONG byte_offset,
                                   ULONGLONG length) const noexcept;

  // Reads $MFT's DATA at a byte offset, following extent boundaries when it is
  // split across extension records.
  [[nodiscard]] std::optional<ULONGLONG> ReadData(ULONGLONG offset,
                                                  std::span<BYTE> buffer) const;

  // File records that $MFT's declared size and its mapped extents both cover.
  [[nodiscard]] ULONGLONG RecordCount() const noexcept;

 private:
  // One VCN range $MFT's own DATA attribute maps: base extent or continuation.
  struct Extent {
    ULONGLONG start_vcn;
    ULONGLONG last_vcn;
    const Attr::AttrBase<S>* attr;
  };

  // The extent covering vcn, or nullptr if no extent maps it.
  [[nodiscard]] const Extent* FindExtent(ULONGLONG vcn) const noexcept;

  DWORD cluster_size_;
  DWORD file_record_size_;

  // $MFT's base DATA attribute. Null until SetBase(). Its declared size caps
  // RecordCount().
  const Attr::AttrBase<S>* base_{nullptr};

  // Sorted by start_vcn; binary-searched per file-record read.
  std::vector<Extent> extents_;
};

}  // namespace NtfsBrowser::Mft
