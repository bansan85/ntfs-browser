#pragma once

#include <optional>
#include <span>
#include <vector>

#include <windows.h>

#include <gsl/pointers>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
template <Strategy S>
class FileRecord;
template <Strategy S>
class NtfsVolume;

template <Strategy S>
class AttrBase
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

 public:
  [[nodiscard]] const AttrHeaderCommon& GetAttrHeader() const noexcept;
  [[nodiscard]] AttrType GetAttrType() const noexcept;
  [[nodiscard]] DWORD GetAttrTotalSize() const noexcept;
  [[nodiscard]] bool IsNonResident() const noexcept;
  [[nodiscard]] WORD GetAttrFlags() const noexcept;
  [[nodiscard]] std::wstring_view GetAttrName() const;
  [[nodiscard]] bool IsUnNamed() const noexcept;

 protected:
  [[nodiscard]] virtual bool IsDataRunOK() const noexcept = 0;

  [[nodiscard]] WORD GetSectorSize() const noexcept;
  [[nodiscard]] DWORD GetClusterSize() const noexcept;
  [[nodiscard]] DWORD GetIndexBlockSize() const noexcept;

 public:
  [[nodiscard]] virtual const BYTE* GetData() const noexcept = 0;
  [[nodiscard]] virtual ULONGLONG GetDataSize() const noexcept = 0;
  [[nodiscard]] virtual std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, const std::span<BYTE>& buffer) const = 0;
};  // AttrBase

}  // namespace NtfsBrowser
