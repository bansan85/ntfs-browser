#pragma once

#include <optional>
#include <span>
#include <vector>

#include <windows.h>

#include <gsl/pointers>

#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser
{
class FileRecord;
class NtfsVolume;

class AttrBase
{
 public:
  AttrBase(const AttrHeaderCommon& ahc, const FileRecord& fr) noexcept;
  AttrBase(AttrBase&& other) noexcept = delete;
  AttrBase(AttrBase const& other) = delete;
  AttrBase& operator=(AttrBase&& other) noexcept = delete;
  AttrBase& operator=(AttrBase const& other) = delete;
  virtual ~AttrBase() = default;

 protected:
  const AttrHeaderCommon& attr_header_;
  const NtfsVolume& volume_;

 public:
  [[nodiscard]] const AttrHeaderCommon& GetAttrHeader() const noexcept;
  [[nodiscard]] DWORD GetAttrType() const noexcept;
  [[nodiscard]] DWORD GetAttrTotalSize() const noexcept;
  [[nodiscard]] bool IsNonResident() const noexcept;
  [[nodiscard]] WORD GetAttrFlags() const noexcept;
  [[nodiscard]] std::wstring GetAttrName() const;
  [[nodiscard]] bool IsUnNamed() const noexcept;

 protected:
  [[nodiscard]] virtual bool IsDataRunOK() const noexcept = 0;

  [[nodiscard]] WORD GetSectorSize() const noexcept;
  [[nodiscard]] DWORD GetClusterSize() const noexcept;
  [[nodiscard]] DWORD GetIndexBlockSize() const noexcept;

 public:
  [[nodiscard]] virtual ULONGLONG GetDataSize() const noexcept = 0;
  [[nodiscard]] virtual std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, std::span<BYTE> buffer) const = 0;
};  // AttrBase

}  // namespace NtfsBrowser
