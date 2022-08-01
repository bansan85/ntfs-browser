#pragma once

#include <ntfs-browser/data/attr-filename.h>

namespace NtfsBrowser
{

////////////////////////////////////////
// FileName helper class
// used by FileName and IndexEntry
////////////////////////////////////////
class FileName
{
 public:
  FileName(Data::AttrFilename* fn = NULL);
  virtual ~FileName();

 protected:
  const Data::AttrFilename* filename_;  // May be NULL for an IndexEntry
  wchar_t*
      FileNameWUC;  // Uppercase Unicode File Name, used to compare file names
  int FileNameLength;
  BOOL IsCopy;

  void SetFileName(Data::AttrFilename* fn);
  void FileName::CopyFileName(const FileName* fn,
                              const Data::AttrFilename* afn);

 private:
  void GetFileNameWUC();

 public:
  int Compare(const wchar_t* fn) const;
  int Compare(const char* fn) const;

  ULONGLONG GetFileSize() const;
  DWORD GetFilePermission() const;
  BOOL IsReadOnly() const;
  BOOL IsHidden() const;
  BOOL IsSystem() const;
  BOOL IsDirectory() const;
  BOOL IsCompressed() const;
  BOOL IsEncrypted() const;
  BOOL IsSparse() const;

  int GetFileName(char* buf, DWORD bufLen) const;
  int GetFileName(wchar_t* buf, DWORD bufLen) const;
  BOOL HasName() const;
  BOOL IsWin32Name() const;

  void GetFileTime(FILETIME* writeTm, FILETIME* createTm = NULL,
                   FILETIME* accessTm = NULL) const;
};  // FileName

}  // namespace NtfsBrowser