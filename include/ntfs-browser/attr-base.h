#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/pointers>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
template <Strategy S>
class FileRecord;
template <Strategy S>
class NtfsVolume;

template <Strategy S>
class NTFS_BROWSER_EXPORT AttrBase
{
 public:
  AttrBase(const AttrHeaderCommon& ahc, const FileRecord<S>& fr) noexcept;
  AttrBase(AttrBase&& other) noexcept = delete;
  AttrBase(AttrBase const& other) = delete;
  AttrBase& operator=(AttrBase&& other) noexcept = delete;
  AttrBase& operator=(AttrBase const& other) = delete;
  virtual ~AttrBase() = default;

 protected:
  const AttrHeaderCommon& attr_header_;
  const NtfsVolume<S>& volume_;

 private:
  // GetAttrName()'s decoded name, cached since it is const. The on-disk
  // bytes are raw UTF-16 code units (WORD), which is not what wchar_t is
  // made of once it is wider than 16 bits, so this is an owned decode, not
  // a view into attr_header_. AttrBase is never moved or copied, so a
  // member is safe to alias.
  mutable std::wstring attr_name_cache_;

 public:
  [[nodiscard]] const AttrHeaderCommon& GetAttrHeader() const noexcept;
  [[nodiscard]] AttrType GetAttrType() const noexcept;
  [[nodiscard]] DWORD GetAttrTotalSize() const noexcept;
  [[nodiscard]] bool IsNonResident() const noexcept;
  [[nodiscard]] WORD GetAttrFlags() const noexcept;
  [[nodiscard]] std::wstring_view GetAttrName() const;
  [[nodiscard]] bool IsUnNamed() const noexcept;

 protected:
  [[nodiscard]] WORD GetSectorSize() const noexcept;
  [[nodiscard]] DWORD GetClusterSize() const noexcept;
  [[nodiscard]] DWORD GetIndexBlockSize() const noexcept;

 public:
  [[nodiscard]] virtual const BYTE* GetData() const noexcept = 0;
  [[nodiscard]] virtual ULONGLONG GetDataSize() const noexcept = 0;
  // Bytes actually allocated on disk for this attribute; equals GetDataSize()
  // for a resident attribute, since it has no separate allocation.
  [[nodiscard]] virtual ULONGLONG GetAllocatedSize() const noexcept = 0;
  [[nodiscard]] virtual std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, const std::span<BYTE>& buffer) const = 0;
};  // AttrBase

}  // namespace NtfsBrowser
