#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <unordered_set>

namespace NtfsBrowser
{
enum class Strategy : std::uint8_t;
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

template <typename TYPE_RESIDENT, Strategy S>
class AttrList : public TYPE_RESIDENT
{
 public:
  // attrListChain: (record, attribute type) pairs already resolved along
  // the current $ATTRIBUTE_LIST chain, threaded through every extension
  // record opened along the way.
  AttrList(const AttrHeaderCommon& ahc, FileRecord<S>& file_record,
           std::unordered_set<ULONGLONG>& attrListChain);
  AttrList(AttrList&& other) noexcept = delete;
  AttrList(AttrList const& other) = delete;
  AttrList& operator=(AttrList&& other) noexcept = delete;
  AttrList& operator=(AttrList const& other) = delete;
  ~AttrList() override;
};  // AttrList

}  // namespace NtfsBrowser
