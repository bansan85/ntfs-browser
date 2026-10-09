#pragma once

#include <ntfs-browser/win-types.h>

#include <string>

#include <ntfs-browser/export.h>

namespace NtfsBrowser {
namespace Data {

struct Filename;

}  // namespace Data

namespace Data {

enum class FilenameFlag : DWORD;

}  // namespace Data

// The case mapping NTFS orders names by. An implementation detail: consumers
// only ever meet it through NtfsVolume, which owns the volume's table.
namespace UpCase {

class Table;

}  // namespace UpCase

class NTFS_BROWSER_EXPORT Filename {
 public:
  Filename() = default;
  Filename(Filename&& other) noexcept = default;
  Filename(const Filename& other) = default;
  Filename& operator=(Filename&& other) noexcept = delete;
  Filename& operator=(const Filename& other) = delete;
  virtual ~Filename() = default;

 protected:
  void SetFilename(const Data::Filename& filename);
  void CopyFilename(const Filename& filename, const Data::Filename& afn);

 private:
  // May be NULL for an IndexEntry
  const Data::Filename* filename_{nullptr};
  // The decoded file name, filled in by GetFilename() so Compare() and
  // repeat callers can reuse it without redecoding. Owned, not a view into
  // the on-disk bytes: those are raw UTF-16 code units (WORD), which is not
  // what wchar_t is made of once it is wider than 16 bits. mutable:
  // GetFilename() is const, and is the sole writer.
  mutable std::wstring filename_wuc_;

  void GetFilenameWUC() const;

 public:
  // Orders fn against this name in NTFS' collation order: <0 when fn sorts
  // first, 0 when the names are equal ignoring case, >0 otherwise. Case is
  // folded with the library's built-in mapping, not the volume's own $UpCase
  // table: use the overload below when a volume is at hand.
  [[nodiscard]] int Compare(std::wstring_view file_name) const noexcept;
  // Same, folding case through upcase, the table of the volume this name
  // came from.
  [[nodiscard]] int Compare(std::wstring_view file_name,
                            const UpCase::Table& upcase) const noexcept;

  [[nodiscard]] ULONGLONG GetFileSize() const noexcept;
  // Allocated size of the file, as last mirrored into this $FILE_NAME (NTFS
  // only refreshes it on a rename, so it can lag the $DATA stream's own).
  [[nodiscard]] ULONGLONG GetAllocatedSize() const noexcept;
  // MFT record number of the parent directory this name was filed under.
  [[nodiscard]] ULONGLONG GetParentReference() const noexcept;
  // Sequence number the parent record had when this name was filed. It tells
  // a live parent from a deleted one whose record was since reused.
  [[nodiscard]] WORD GetParentSequenceNumber() const noexcept;
  [[nodiscard]] virtual Data::FilenameFlag GetFilePermission() const noexcept;
  [[nodiscard]] virtual bool IsReadOnly() const noexcept;
  [[nodiscard]] virtual bool IsHidden() const noexcept;
  [[nodiscard]] virtual bool IsSystem() const noexcept;
  [[nodiscard]] virtual bool IsArchive() const noexcept;
  [[nodiscard]] virtual bool IsDirectory() const noexcept;
  [[nodiscard]] virtual bool IsCompressed() const noexcept;
  [[nodiscard]] virtual bool IsEncrypted() const noexcept;
  [[nodiscard]] virtual bool IsSparse() const noexcept;

  [[nodiscard]] std::wstring_view GetFilename() const;
  [[nodiscard]] bool HasName() const noexcept;
  [[nodiscard]] bool IsWin32Name() const noexcept;

  // changeTm is the last MFT (metadata) change time, distinct from writeTm's
  // content modification time.
  virtual void GetFileTime(FILETIME* write_tm, FILETIME* create_tm,
                           FILETIME* access_tm,
                           FILETIME* change_tm = nullptr) const noexcept;
};  // Filename

}  // namespace NtfsBrowser
