#pragma once

// OK
#include <windows.h>

#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser
{
class FileRecord;

class AttrBase
{
 public:
  AttrBase(const AttrHeaderCommon& ahc, const FileRecord& fr);
  AttrBase(AttrBase&& other) noexcept = delete;
  AttrBase(AttrBase const& other) = delete;
  AttrBase& operator=(AttrBase&& other) noexcept = delete;
  AttrBase& operator=(AttrBase const& other) = delete;
  virtual ~AttrBase();

 private:
  const AttrHeaderCommon& attr_header_;
  WORD sector_size_;
  DWORD cluster_size_;
  DWORD index_block_size_;
  HANDLE hvolume_;
  const FileRecord& file_record_;

 public:
  [[nodiscard]] const AttrHeaderCommon& GetAttrHeader() const;
  [[nodiscard]] DWORD GetAttrType() const;
  [[nodiscard]] DWORD GetAttrTotalSize() const;
  [[nodiscard]] BOOL IsNonResident() const;
  [[nodiscard]] WORD GetAttrFlags() const;
  [[nodiscard]] int GetAttrName(char* buf, DWORD bufLen) const;
  [[nodiscard]] int GetAttrName(wchar_t* buf, DWORD bufLen) const;
  [[nodiscard]] BOOL IsUnNamed() const;

 protected:
  [[nodiscard]] virtual BOOL IsDataRunOK() const noexcept = 0;

  [[nodiscard]] WORD GetSectorSize() const;
  [[nodiscard]] DWORD GetClusterSize() const;
  [[nodiscard]] DWORD GetIndexBlockSize() const;
  [[nodiscard]] HANDLE GetHandle() const;

 public:
  [[nodiscard]] virtual ULONGLONG GetDataSize() const = 0;
  [[nodiscard]] virtual BOOL ReadData(ULONGLONG offset, void* bufv,
                                      DWORD bufLen, DWORD& actural) const = 0;
};  // AttrBase

}  // namespace NtfsBrowser
