#pragma once

#include <cstdint>

#include <ntfs-browser/filename.h>
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
