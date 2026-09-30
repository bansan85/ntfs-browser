#pragma once

#include <ntfs-browser/win-types.h>
#include <cstdint>

namespace NtfsBrowser
{
enum class Strategy : std::uint8_t;
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

namespace Attr
{
struct StandardInformation;
}  // namespace Attr
namespace Flag
{
enum class StdInfoPermission : DWORD;
}  // namespace Flag

template <typename RESIDENT, Strategy S>
class AttrStdInfo : public RESIDENT
{
 public:
  AttrStdInfo(const AttrHeaderCommon& ahc, const FileRecord<S>& fr);
  AttrStdInfo(AttrStdInfo&& other) noexcept = delete;
  AttrStdInfo(AttrStdInfo const& other) = delete;
  AttrStdInfo& operator=(AttrStdInfo&& other) noexcept = delete;
  AttrStdInfo& operator=(AttrStdInfo const& other) = delete;
  ~AttrStdInfo() override;

  template <Strategy>
  friend class FileRecord;

 private:
  const Attr::StandardInformation& std_info_;

  void GetFileTime(FILETIME* writeTm, FILETIME* createTm, FILETIME* accessTm,
                   FILETIME* changeTm = nullptr) const noexcept;
  [[nodiscard]] Flag::StdInfoPermission GetFilePermission() const noexcept;
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
}  // namespace NtfsBrowser