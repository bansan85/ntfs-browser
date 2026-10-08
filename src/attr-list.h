#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <unordered_set>

namespace NtfsBrowser {

enum class Strategy : std::uint8_t;
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

namespace Attr {

struct AttributeList;

}  // namespace Attr

template <typename Resident, Strategy S>
class AttrList : public Resident {
 public:
  // attrListChain: (record, attribute type) pairs already resolved along
  // the current $ATTRIBUTE_LIST chain, threaded through every extension
  // record opened along the way.
  AttrList(const AttrHeaderCommon& ahc, FileRecord<S>& file_record,
           std::unordered_set<ULONGLONG>& attr_list_chain);
  AttrList(AttrList&& other) noexcept = delete;
  AttrList(const AttrList& other) = delete;
  AttrList& operator=(AttrList&& other) noexcept = delete;
  AttrList& operator=(const AttrList& other) = delete;
  ~AttrList() override;

 private:
  static void ResolveEntry(const Attr::AttributeList& entry,
                           FileRecord<S>& file_record,
                           std::unordered_set<ULONGLONG>& attr_list_chain,
                           bool recover);
};  // AttrList

}  // namespace NtfsBrowser
