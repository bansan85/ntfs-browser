#pragma once

#include <vector>

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/strategy.h>

#include "attr-resident.h"

namespace NtfsBrowser
{

namespace Attr
{
struct IndexRoot;
}  // namespace Attr

template <typename RESIDENT, Strategy S>
class AttrIndexRoot : public RESIDENT, public std::vector<IndexEntry>
{
 public:
  AttrIndexRoot(const AttrHeaderCommon& ahc, const FileRecord<S>& fr);
  AttrIndexRoot(AttrIndexRoot&& other) noexcept = delete;
  AttrIndexRoot(AttrIndexRoot const& other) = delete;
  AttrIndexRoot& operator=(AttrIndexRoot&& other) noexcept = delete;
  AttrIndexRoot& operator=(AttrIndexRoot const& other) = delete;
  ~AttrIndexRoot() override;

 private:
  const Attr::IndexRoot* index_root_;

  void ParseIndexEntries();

 public:
  [[nodiscard]] bool IsFileName() const noexcept;
};  // AttrIndexRoot

}  // namespace NtfsBrowser