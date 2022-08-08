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
  wchar_t* filename_wuc_;
  int filename_length_;
  bool is_copy_;

  void SetFilename(const Attr::Filename* fn);
  void CopyFilename(const Filename* fn, const Attr::Filename* afn);

 private:
  void GetFilenameWUC();

 public:
  int Compare(const wchar_t* fn) const;
  int Compare(const char* fn) const;

  ULONGLONG GetFileSize() const;
  virtual Flag::Filename GetFilePermission() const noexcept;
  virtual bool IsReadOnly() const noexcept;
  virtual bool IsHidden() const noexcept;
  virtual bool IsSystem() const noexcept;
  virtual bool IsDirectory() const noexcept;
  virtual bool IsCompressed() const noexcept;
  virtual bool IsEncrypted() const noexcept;
  virtual bool IsSparse() const noexcept;

  int GetFilename(char* buf, DWORD bufLen) const;
  int GetFilename(wchar_t* buf, DWORD bufLen) const;
  bool HasName() const;
  bool IsWin32Name() const;

  virtual void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                           FILETIME* accessTm) const noexcept;
};  // Filename

}  // namespace NtfsBrowser