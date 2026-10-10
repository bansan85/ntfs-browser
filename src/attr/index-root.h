#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <vector>

#include <ntfs-browser/io/file-record.h>

namespace NtfsBrowser {

class IndexEntryView;

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Data {

struct IndexRoot;

}  // namespace Data

namespace Attr {

template <typename Resident, Cache::Strategy S>
class AttrIndexRoot : public Resident, public std::vector<IndexEntryView> {
 public:
  AttrIndexRoot(const HeaderCommon& ahc, const Io::FileRecord<S>& file_record);
  AttrIndexRoot(AttrIndexRoot&& other) noexcept = delete;
  AttrIndexRoot(const AttrIndexRoot& other) = delete;
  AttrIndexRoot& operator=(AttrIndexRoot&& other) noexcept = delete;
  AttrIndexRoot& operator=(const AttrIndexRoot& other) = delete;
  ~AttrIndexRoot() override;

  template <Cache::Strategy>
  friend class NtfsBrowser::Io::FileRecord;

 private:
  const Data::IndexRoot* index_root_;
  // A private copy of the resident data, which the entries are views into.
  std::vector<BYTE> index_data_;
  // Aligned copies of the entries that sit at a misaligned address in
  // index_data_.
  std::vector<std::vector<BYTE>> realigned_;

  [[nodiscard]] bool ParseIndexEntries();
  [[nodiscard]] bool IsFileName() const noexcept;
};  // AttrIndexRoot

}  // namespace Attr

}  // namespace NtfsBrowser
