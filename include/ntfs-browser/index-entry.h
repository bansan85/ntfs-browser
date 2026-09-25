#pragma once

#include <memory>
#include <optional>

#include <ntfs-browser/export.h>
#include <ntfs-browser/filename.h>

namespace NtfsBrowser
{
namespace Data
{
struct IndexEntry;
}  // namespace Data

class NTFS_BROWSER_EXPORT IndexEntry : public Filename
{
 public:
  explicit IndexEntry(std::shared_ptr<BYTE[]> sh_ptr,
                      const Data::IndexEntry& ie);
  IndexEntry(IndexEntry&& other) noexcept = default;
  IndexEntry(IndexEntry const& other) = default;
  IndexEntry& operator=(IndexEntry&& other) noexcept = delete;
  IndexEntry& operator=(IndexEntry const& other) = delete;
  ~IndexEntry() override = default;

 private:
  std::shared_ptr<BYTE[]> sh_ptr_;
  const Data::IndexEntry& index_entry_;

 public:
  [[nodiscard]] ULONGLONG GetFileReference() const noexcept;
  // Times the record this entry names has been reused, read from the
  // entry's own mft_sn field - not the live record's current sequence,
  // which may already differ.
  [[nodiscard]] WORD GetSequenceNumber() const noexcept;
  [[nodiscard]] bool IsSubNodePtr() const noexcept;
  [[nodiscard]] ULONGLONG GetSubNodeVCN() const noexcept;
};  // IndexEntry

}  // namespace NtfsBrowser