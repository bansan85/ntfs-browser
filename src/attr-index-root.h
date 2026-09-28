#pragma once

#include <vector>

namespace NtfsBrowser
{
class IndexEntry;
enum class Strategy;
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

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

  template <Strategy>
  friend class FileRecord;

 private:
  const Attr::IndexRoot* index_root_;

  [[nodiscard]] bool ParseIndexEntries();
  [[nodiscard]] bool IsFileName() const noexcept;
};  // AttrIndexRoot

}  // namespace NtfsBrowser