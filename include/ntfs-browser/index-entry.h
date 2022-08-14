#pragma once

#include <memory>
#include <optional>

#include <ntfs-browser/filename.h>

namespace NtfsBrowser
{
namespace Data
{
struct IndexEntry;
}  // namespace Data

class IndexEntry : public Filename
{
 public:
  explicit IndexEntry(std::optional<std::shared_ptr<BYTE[]>> sh_ptr,
                      const Data::IndexEntry& ie);
  IndexEntry(IndexEntry&& other) noexcept = default;
  IndexEntry(IndexEntry const& other) = default;
  IndexEntry& operator=(IndexEntry&& other) noexcept = default;
  IndexEntry& operator=(IndexEntry const& other) = default;
  ~IndexEntry() override = default;

 private:
  std::optional<std::shared_ptr<BYTE[]>> sh_ptr_;
  const Data::IndexEntry& index_entry_;

 public:
  [[nodiscard]] ULONGLONG GetFileReference() const noexcept;
  [[nodiscard]] bool IsSubNodePtr() const noexcept;
  [[nodiscard]] ULONGLONG GetSubNodeVCN() const noexcept;
};  // IndexEntry

}  // namespace NtfsBrowser