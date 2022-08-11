#pragma once

#include <windows.h>

#include <gsl/pointers>

#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser
{
class FileRecord;

class AttrBase
{
 public:
  AttrBase(const AttrHeaderCommon& ahc, const FileRecord& fr) noexcept;
  AttrBase(AttrBase&& other) noexcept = delete;
  AttrBase(AttrBase const& other) = delete;
  AttrBase& operator=(AttrBase&& other) noexcept = delete;
  AttrBase& operator=(AttrBase const& other) = delete;
  virtual ~AttrBase() = default;

 private:
  const AttrHeaderCommon& attr_header_;
  WORD sector_size_;
  DWORD cluster_size_;
  DWORD index_block_size_;
  HANDLE hvolume_;

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
  [[nodiscard]] HANDLE GetHandle() const noexcept;

 public:
  [[nodiscard]] virtual ULONGLONG GetDataSize() const noexcept = 0;
  [[nodiscard]] virtual bool ReadData(ULONGLONG offset,
                                      gsl::not_null<void*> bufv,
                                      ULONGLONG bufLen,
                                      ULONGLONG& actural) const = 0;
};  // AttrBase

}  // namespace NtfsBrowser
