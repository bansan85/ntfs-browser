#pragma once

#include <windows.h>

namespace NtfsBrowser
{
namespace Attr
{
struct Filename;
}  // namespace Attr

namespace Flag
{
enum class Filename : DWORD;
}  // namespace Flag

class Filename
{
 public:
  Filename();
  virtual ~Filename();

 protected:
  const Attr::Filename* filename_;  // May be NULL for an IndexEntry
  // Uppercase Unicode File Name, used to compare file names
  wchar_t* FilenameWUC;
  int FilenameLength;
  BOOL IsCopy;

  void SetFilename(Attr::Filename* fn);
  void CopyFilename(const Filename* fn, const Attr::Filename* afn);

 private:
  void GetFilenameWUC();

 public:
  int Compare(const wchar_t* fn) const;
  int Compare(const char* fn) const;

  ULONGLONG GetFileSize() const;
  Flag::Filename GetFilePermission() const;
  BOOL IsReadOnly() const;
  BOOL IsHidden() const;
  BOOL IsSystem() const;
  BOOL IsDirectory() const;
  BOOL IsCompressed() const;
  BOOL IsEncrypted() const;
  BOOL IsSparse() const;

  int GetFilename(char* buf, DWORD bufLen) const;
  int GetFilename(wchar_t* buf, DWORD bufLen) const;
  BOOL HasName() const;
  BOOL IsWin32Name() const;

  void GetFileTime(FILETIME* writeTm, FILETIME* createTm = NULL,
                   FILETIME* accessTm = NULL) const;
};  // Filename

}  // namespace NtfsBrowser