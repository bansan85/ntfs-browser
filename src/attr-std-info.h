#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>

namespace NtfsBrowser {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Attr {

struct HeaderCommon;

}  // namespace Attr
template <Cache::Strategy S>
class FileRecord;

namespace Data {

struct StandardInformation;

}  // namespace Data

namespace Data {

enum class StdInfoPermission : DWORD;

}  // namespace Data

namespace Attr {

template <typename Resident, Cache::Strategy S>
class AttrStdInfo : public Resident {
 public:
  AttrStdInfo(const HeaderCommon& ahc, const FileRecord<S>& file_record);
  AttrStdInfo(AttrStdInfo&& other) noexcept = delete;
  AttrStdInfo(const AttrStdInfo& other) = delete;
  AttrStdInfo& operator=(AttrStdInfo&& other) noexcept = delete;
  AttrStdInfo& operator=(const AttrStdInfo& other) = delete;
  ~AttrStdInfo() override;

  template <Cache::Strategy>
  friend class NtfsBrowser::FileRecord;

 private:
  const Data::StandardInformation& std_info_;

  void GetFileTime(FILETIME* write_tm, FILETIME* create_tm, FILETIME* access_tm,
                   FILETIME* change_tm = nullptr) const noexcept;
  [[nodiscard]] Data::StdInfoPermission GetFilePermission() const noexcept;
  [[nodiscard]] bool IsReadOnly() const noexcept;
  [[nodiscard]] bool IsHidden() const noexcept;
  [[nodiscard]] bool IsSystem() const noexcept;
  [[nodiscard]] bool IsArchive() const noexcept;
  [[nodiscard]] bool IsDevice() const noexcept;
  [[nodiscard]] bool IsNormal() const noexcept;
  [[nodiscard]] bool IsTemporary() const noexcept;
  [[nodiscard]] bool IsSparse() const noexcept;
  [[nodiscard]] bool IsReparsePoint() const noexcept;
  [[nodiscard]] bool IsCompressed() const noexcept;
  [[nodiscard]] bool IsOffline() const noexcept;
  [[nodiscard]] bool IsNotContentIndexed() const noexcept;
  [[nodiscard]] bool IsEncrypted() const noexcept;

 public:
  // Also used by Filename (src/filename.cpp) for $FILE_NAME timestamps.
  static void UTC2Local(const ULONGLONG& ultm, FILETIME& lftm) noexcept;
};  // AttrStdInfo

}  // namespace Attr

}  // namespace NtfsBrowser
