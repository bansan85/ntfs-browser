#pragma once

#include <windows.h>

#include "attr-resident.h"
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

//OK

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

///////////////////////////////////
// Attribute: Standard Information
///////////////////////////////////
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
  [[nodiscard]] BOOL IsReadOnly() const noexcept;
  [[nodiscard]] BOOL IsHidden() const noexcept;
  [[nodiscard]] BOOL IsSystem() const noexcept;
  [[nodiscard]] BOOL IsCompressed() const noexcept;
  [[nodiscard]] BOOL IsEncrypted() const noexcept;
  [[nodiscard]] BOOL IsSparse() const noexcept;

  static void UTC2Local(const ULONGLONG& ultm, FILETIME& lftm) noexcept;
};  // AttrStdInfo
}  // namespace NtfsBrowser