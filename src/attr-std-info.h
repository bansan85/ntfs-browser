#pragma once

#include <windows.h>

#include "attr-resident.h"
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

namespace NtfsBrowser
{
namespace Attr
{
struct StandardInformation;
}  // namespace Attr
namespace Flag
{
enum class StdInfoPermission : DWORD;
}  // namespace Flag

class AttrStdInfo : public AttrResident
{
 public:
  AttrStdInfo(const AttrHeaderCommon& ahc, const FileRecord& fr);
  AttrStdInfo(AttrStdInfo&& other) noexcept = delete;
  AttrStdInfo(AttrStdInfo const& other) = delete;
  AttrStdInfo& operator=(AttrStdInfo&& other) noexcept = delete;
  AttrStdInfo& operator=(AttrStdInfo const& other) = delete;
  ~AttrStdInfo() override;

 private:
  const Attr::StandardInformation& std_info_;

 public:
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                   FILETIME* accessTm) const noexcept;
  [[nodiscard]] Flag::StdInfoPermission GetFilePermission() const noexcept;
  [[nodiscard]] bool IsReadOnly() const noexcept;
  [[nodiscard]] bool IsHidden() const noexcept;
  [[nodiscard]] bool IsSystem() const noexcept;
  [[nodiscard]] bool IsCompressed() const noexcept;
  [[nodiscard]] bool IsEncrypted() const noexcept;
  [[nodiscard]] bool IsSparse() const noexcept;

  static void UTC2Local(const ULONGLONG& ultm, FILETIME& lftm) noexcept;
};  // AttrStdInfo
}  // namespace NtfsBrowser