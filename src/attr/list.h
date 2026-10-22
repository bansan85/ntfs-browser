#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <unordered_set>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

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
  bool ReadEntryHeader(ULONGLONG offset, Data::AttributeList& entry,
                       bool recover, bool& truncated);
};  // AttrList

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
