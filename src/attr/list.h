#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "attr/forward.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Data {

struct AttributeList;

}  // namespace Data

namespace Attr {

template <typename Resident, Cache::Strategy S>
class AttrList : public Resident {
 public:
  // attrListChain: (record, attribute type) pairs already resolved along
  // the current $ATTRIBUTE_LIST chain, threaded through every extension
  // record opened along the way.
  AttrList(const HeaderCommon& ahc, Io::FileRecord<S>& file_record,
           std::unordered_set<ULONGLONG>& attr_list_chain);
  AttrList(AttrList&& other) noexcept = delete;
  AttrList(const AttrList& other) = delete;
  AttrList& operator=(AttrList&& other) noexcept = delete;
  AttrList& operator=(const AttrList& other) = delete;
  ~AttrList() override;

 private:
  // The clusters of a non-resident list the walk has read, kept so that no
  // entry reads them again. With no bytes, each read goes to ReadData().
  struct ListCache {
    std::vector<BYTE> bytes;
    // One flag per cluster of bytes.
    std::vector<bool> loaded;
  };

  // The state of one walk over the entries of the list.
  struct ListWalk {
    ListCache cache;
    // Salvage conditions instead of rejecting the list.
    bool recover;
    // Garbage in the list is no defect: another file may have overwritten it.
    bool overwritable;
    // The walk ended at a truncated entry or off the declared size.
    bool truncated{false};
    // Where the next entry starts, in bytes from the start of the list.
    ULONGLONG offset{0};
  };

  // What the walk does after one entry.
  enum class EntryStep : std::uint8_t {
    Next,        // Read the next entry.
    Stop,        // The entry is too short to step over: end the walk.
    Overwritten  // The list is another file's garbage: end the walk.
  };

  [[nodiscard]] ListCache MakeListCache() const;
  [[nodiscard]] std::optional<ULONGLONG> ReadListBytes(ULONGLONG offset,
                                                       std::span<BYTE> out,
                                                       ListCache& cache) const;
  bool ReadEntryHeader(ULONGLONG offset, Data::AttributeList& entry,
                       ListWalk& walk);

  // Whether garbage in the list is no defect, as ListWalk::overwritable says.
  [[nodiscard]] static bool
      CanBeOverwritten(const Io::FileRecord<S>& file_record);
  [[nodiscard]] bool
      ImportEntries(Io::FileRecord<S>& file_record,
                    std::unordered_set<ULONGLONG>& attr_list_chain,
                    ListWalk& walk);
  [[nodiscard]] static EntryStep ImportEntry(
      const Data::AttributeList& entry, Io::FileRecord<S>& file_record,
      std::unordered_set<ULONGLONG>& attr_list_chain, ListWalk& walk);
  void CheckWalkEnd(Io::FileRecord<S>& file_record, ListWalk& walk);
};  // AttrList

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
