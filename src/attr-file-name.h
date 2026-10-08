#pragma once

#include <cstdint>

#include <ntfs-browser/filename.h>

namespace NtfsBrowser {

struct AttrHeaderCommon;
enum class Strategy : std::uint8_t;
template <Strategy S>
class FileRecord;

template <typename Resident, Strategy S>
class AttrFileName : public Resident, public Filename {
 public:
  AttrFileName(const AttrHeaderCommon& ahc, const FileRecord<S>& file_record);
  AttrFileName(AttrFileName&& other) noexcept = delete;
  AttrFileName(const AttrFileName& other) = delete;
  AttrFileName& operator=(AttrFileName&& other) noexcept = delete;
  AttrFileName& operator=(const AttrFileName& other) = delete;
  ~AttrFileName() override;
};  // AttrFileName

}  // namespace NtfsBrowser
