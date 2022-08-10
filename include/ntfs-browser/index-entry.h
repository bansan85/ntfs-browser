#pragma once

#include <ntfs-browser/filename.h>

// OK

namespace NtfsBrowser
{
namespace Data
{
struct IndexEntry;
}  // namespace Data

class IndexEntry : public Filename
{
 public:
  IndexEntry(const Data::IndexEntry& ie);
  IndexEntry(IndexEntry&& other) noexcept = default;
  IndexEntry(IndexEntry const& other) = default;
  IndexEntry& operator=(IndexEntry&& other) noexcept = default;
  IndexEntry& operator=(IndexEntry const& other) = default;
  virtual ~IndexEntry() = default;

 protected:
  const Data::IndexEntry& index_entry_;

 public:
  ULONGLONG GetFileReference() const noexcept;
  bool IsSubNodePtr() const noexcept;
  ULONGLONG GetSubNodeVCN() const noexcept;
};  // IndexEntry

}  // namespace NtfsBrowser