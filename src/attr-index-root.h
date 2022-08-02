#pragma once

#include <vector>

#include "attr-resident.h"
#include <ntfs-browser/index-entry.h>

namespace NtfsBrowser
{

namespace Attr
{
struct IndexRoot;
}  // namespace Attr

/////////////////////////////////////
// Attribute: Index Root (Resident)
/////////////////////////////////////
class AttrIndexRoot : public AttrResident, public std::vector<IndexEntry*>
{
 public:
  AttrIndexRoot(const AttrHeaderCommon* ahc, const FileRecord* fr);
  virtual ~AttrIndexRoot();

 private:
  const Attr::IndexRoot* IndexRoot;

  void ParseIndexEntries();

 public:
  BOOL IsFileName() const;
};  // AttrIndexRoot

}  // namespace NtfsBrowser