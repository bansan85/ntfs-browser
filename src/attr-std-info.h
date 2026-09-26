#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

#include "attr-resident.h"

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

  // FileRecord dispatches through both Strategy instantiations of this
  // class via a runtime if/else on S, not `if constexpr`, so every
  // FileRecord<S> must be a friend regardless of this instantiation's own S.
  template <Strategy>
  friend class FileRecord;

 private:
  const Attr::StandardInformation& std_info_;

  void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                   FILETIME* accessTm) const noexcept;
  [[nodiscard]] Flag::StdInfoPermission GetFilePermission() const noexcept;
  [[nodiscard]] bool IsReadOnly() const noexcept;
  [[nodiscard]] bool IsHidden() const noexcept;
  [[nodiscard]] bool IsSystem() const noexcept;
  [[nodiscard]] bool IsCompressed() const noexcept;
  [[nodiscard]] bool IsEncrypted() const noexcept;
  [[nodiscard]] bool IsSparse() const noexcept;

 public:
  // Also used by Filename (src/filename.cpp) for $FILE_NAME timestamps.
  static void UTC2Local(const ULONGLONG& ultm, FILETIME& lftm) noexcept;
};  // AttrStdInfo
}  // namespace NtfsBrowser