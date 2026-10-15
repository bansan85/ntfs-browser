#pragma once

#include <cstdint>

#include <ntfs-browser/filename.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Attr {

template <typename Resident, Cache::Strategy S>
class AttrFileName : public Resident, public NTFS_BROWSER_NAMESPACE::Filename {
 public:
  AttrFileName(const HeaderCommon& ahc, const Io::FileRecord<S>& file_record);
  AttrFileName(AttrFileName&& other) noexcept = delete;
  AttrFileName(const AttrFileName& other) = delete;
  AttrFileName& operator=(AttrFileName&& other) noexcept = delete;
  AttrFileName& operator=(const AttrFileName& other) = delete;
  ~AttrFileName() override;
};  // AttrFileName

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
