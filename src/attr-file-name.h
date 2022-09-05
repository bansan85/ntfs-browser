#pragma once

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/filename.h>

#include "attr-resident.h"

namespace NtfsBrowser
{
struct AttrHeaderCommon;

template <typename RESIDENT>
class AttrFileName : public RESIDENT, public Filename
{
 public:
  AttrFileName(const AttrHeaderCommon& ahc, const FileRecord& fr);
  AttrFileName(AttrFileName&& other) noexcept = delete;
  AttrFileName(AttrFileName const& other) = delete;
  AttrFileName& operator=(AttrFileName&& other) noexcept = delete;
  AttrFileName& operator=(AttrFileName const& other) = delete;
  ~AttrFileName() override;

 private:
  // File permission and time in $FILE_NAME only updates when the filename changes
  // So hide these functions to prevent user from getting the error information
  // Standard Information and IndexEntry keeps the most recent file time and permission infomation
#if 0
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                   FILETIME* accessTm) const noexcept override;
  [[nodiscard]] Flag::Filename GetFilePermission() const noexcept override;
  [[nodiscard]] bool IsReadOnly() const noexcept override;
  [[nodiscard]] bool IsHidden() const noexcept override;
  [[nodiscard]] bool IsSystem() const noexcept override;
  [[nodiscard]] bool IsCompressed() const noexcept override;
  [[nodiscard]] bool IsEncrypted() const noexcept override;
  [[nodiscard]] bool IsSparse() const noexcept override;
#endif
};  // AttrFileName

}  // namespace NtfsBrowser