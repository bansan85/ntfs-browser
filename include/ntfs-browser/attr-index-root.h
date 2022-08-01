#pragma once

#include <vector>

#include <ntfs-browser/attr-resident.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/data/attr-index-root.h>

namespace NtfsBrowser
{

/////////////////////////////////////
// Attribute: Index Root (Resident)
/////////////////////////////////////
class AttrIndexRoot : public AttrResident, public std::vector<IndexEntry*>
{
 public:
  AttrIndexRoot(const AttrHeaderCommon* ahc, const FileRecord* fr);
  virtual ~AttrIndexRoot();

 private:
  const Data::AttrIndexRoot* IndexRoot;

  void ParseIndexEntries();

 public:
  BOOL IsFileName() const;
};  // AttrIndexRoot

}  // namespace NtfsBrowser