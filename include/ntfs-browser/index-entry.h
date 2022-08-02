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
  IndexEntry();
  IndexEntry(const Data::IndexEntry* ie);
  virtual ~IndexEntry();

 private:
  BOOL IsDefault;

 protected:
  const Data::IndexEntry* index_entry_;

 public:
  // Use with caution !
  IndexEntry& operator=(const IndexEntry& ieClass);
  ULONGLONG GetFileReference() const;
  BOOL IsSubNodePtr() const;
  ULONGLONG GetSubNodeVCN() const;
};  // IndexEntry

}  // namespace NtfsBrowser