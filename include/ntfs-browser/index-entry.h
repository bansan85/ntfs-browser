#pragma once

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
  IndexEntry(const Data::IndexEntry* ie);
  virtual ~IndexEntry() = default;

 private:
  bool is_default_;

 protected:
  const Data::IndexEntry* index_entry_;

 public:
  // Use with caution !
  IndexEntry& operator=(const IndexEntry& ieClass);
  ULONGLONG GetFileReference() const;
  bool IsSubNodePtr() const;
  ULONGLONG GetSubNodeVCN() const;
};  // IndexEntry

}  // namespace NtfsBrowser