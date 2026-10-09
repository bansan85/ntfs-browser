#pragma once

#include <cstdint>

#include <ntfs-browser/filename.h>

namespace NtfsBrowser {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache
template <Cache::Strategy S>
class FileRecord;

namespace Attr {

template <typename Resident, Cache::Strategy S>
class AttrFileName : public Resident, public NtfsBrowser::Filename {
 public:
  AttrFileName(const HeaderCommon& ahc, const FileRecord<S>& file_record);
  AttrFileName(AttrFileName&& other) noexcept = delete;
  AttrFileName(const AttrFileName& other) = delete;
  AttrFileName& operator=(AttrFileName&& other) noexcept = delete;
  AttrFileName& operator=(const AttrFileName& other) = delete;
  ~AttrFileName() override;
};  // AttrFileName

}  // namespace Attr

}  // namespace NtfsBrowser
