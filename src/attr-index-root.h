#pragma once

#include <vector>

#include <ntfs-browser/index-entry.h>

#include "attr-resident.h"

// OK

namespace NtfsBrowser
{

namespace Attr
{
struct IndexRoot;
}  // namespace Attr

/////////////////////////////////////
// Attribute: Index Root (Resident)
/////////////////////////////////////
class AttrIndexRoot : public AttrResident, public std::vector<IndexEntry>
{
 public:
  AttrIndexRoot(const AttrHeaderCommon& ahc, const FileRecord& fr);
  AttrIndexRoot(AttrIndexRoot&& other) noexcept = delete;
  AttrIndexRoot(AttrIndexRoot const& other) = delete;
  AttrIndexRoot& operator=(AttrIndexRoot&& other) noexcept = delete;
  AttrIndexRoot& operator=(AttrIndexRoot const& other) = delete;
  virtual ~AttrIndexRoot();

 private:
  const Attr::IndexRoot* index_root_;

  void ParseIndexEntries();

 public:
  bool IsFileName() const noexcept;
};  // AttrIndexRoot

}  // namespace NtfsBrowser