#include <crtdbg.h>

#include "ntfs-common.h"
#include <ntfs-browser/filename.h>
#include "attr/filename.h"
#include "flag/filename.h"
#include "flag/filename-namespace.h"
#include "attr-std-info.h"

namespace NtfsBrowser
{

Filename::Filename()
{
  is_copy_ = FALSE;

  filename_ = nullptr;

  filename_wuc_ = nullptr;
  filename_length_ = 0;
}

Filename::~Filename()
{
  if (filename_wuc_) delete filename_wuc_;
}

void Filename::SetFilename(const Attr::Filename* fn)
{
  filename_ = fn;

  GetFilenameWUC();
}

// Copy pointer buffers
void Filename::CopyFilename(const Filename* fn, const Attr::Filename* afn)
{
  if (!is_copy_)
  {
    NTFS_TRACE("Cannot call this routine\n");
    return;
  }

  _ASSERT(fn && afn);

  NTFS_TRACE("Filename Copied\n");

  if (filename_wuc_)
  {
    delete filename_wuc_;
    filename_wuc_ = nullptr;
  }

  filename_length_ = fn->filename_length_;
  filename_ = afn;

  if (fn->filename_wuc_)
  {
    filename_wuc_ = new wchar_t[filename_length_ + 1];
    wcsncpy(filename_wuc_, fn->filename_wuc_, filename_length_);
    filename_wuc_[filename_length_] = wchar_t('\0');
  }
  else
    filename_wuc_ = nullptr;
}

// Get uppercase unicode filename and store it in a buffer
void Filename::GetFilenameWUC()
{
#ifdef _DEBUG
  char fna[MAX_PATH];
  GetFilename(fna, MAX_PATH);  // Just show filename in debug window
#endif

  if (filename_wuc_)
  {
    delete filename_wuc_;
    filename_wuc_ = nullptr;
    filename_length_ = 0;
  }

  wchar_t fns[MAX_PATH];
  filename_length_ = GetFilename(fns, MAX_PATH);

  if (filename_length_ > 0)
  {
    filename_wuc_ = new wchar_t[filename_length_ + 1];
    for (int i = 0; i < filename_length_; i++)
      filename_wuc_[i] = towupper(fns[i]);
    filename_wuc_[filename_length_] = wchar_t('\0');
  }
  else
  {
    filename_length_ = 0;
    filename_wuc_ = nullptr;
  }
}

// Compare Unicode file name
int Filename::Compare(const wchar_t* fn) const
{
  // Change fn to upper case
  size_t len = wcslen(fn);
  if (len >= MAX_PATH) return 1;  // Assume bigger

  wchar_t fns[MAX_PATH];

  for (int i = 0; i < len; i++) fns[i] = towupper(fn[i]);
  fns[len] = wchar_t('\0');

  return wcscmp(fns, filename_wuc_);
}

// Compare ANSI file name
int Filename::Compare(const char* fn) const
{
  wchar_t fnw[MAX_PATH];

  int len = MultiByteToWideChar(CP_ACP, 0, fn, -1, fnw, MAX_PATH);
  if (len)
    return Compare(fnw);
  else
    return 1;  // Assume bigger
}

ULONGLONG Filename::GetFileSize() const
{
  return filename_ ? filename_->RealSize : 0;
}

Flag::Filename Filename::GetFilePermission() const noexcept
{
  return filename_ ? filename_->flags : Flag::Filename::NONE;
}

bool Filename::IsReadOnly() const noexcept
{
  return filename_
             ? static_cast<bool>(filename_->flags & Flag::Filename::READONLY)
             : FALSE;
}

bool Filename::IsHidden() const noexcept
{
  return filename_
             ? static_cast<bool>(filename_->flags & Flag::Filename::HIDDEN)
             : FALSE;
}

bool Filename::IsSystem() const noexcept
{
  return filename_
             ? static_cast<bool>(filename_->flags & Flag::Filename::SYSTEM)
             : FALSE;
}

bool Filename::IsDirectory() const noexcept
{
  return filename_
             ? static_cast<bool>(filename_->flags & Flag::Filename::DIRECTORY)
             : FALSE;
}

bool Filename::IsCompressed() const noexcept
{
  return filename_
             ? static_cast<bool>(filename_->flags & Flag::Filename::COMPRESSED)
             : FALSE;
}

bool Filename::IsEncrypted() const noexcept
{
  return filename_
             ? static_cast<bool>(filename_->flags & Flag::Filename::ENCRYPTED)
             : FALSE;
}

bool Filename::IsSparse() const noexcept
{
  return filename_
             ? static_cast<bool>(filename_->flags & Flag::Filename::SPARSE)
             : FALSE;
}

// Get ANSI File Name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
int Filename::GetFilename(char* buf, DWORD bufLen) const
{
  if (filename_ == nullptr) return 0;

  int len = 0;

  if (filename_->name_length)
  {
    if (bufLen < filename_->name_length)
      return -1 * filename_->name_length;  // buffer too small

    len = WideCharToMultiByte(CP_ACP, 0, (wchar_t*)filename_->Name,
                              filename_->name_length, buf, bufLen, nullptr,
                              nullptr);
    if (len)
    {
      buf[len] = '\0';
      NTFS_TRACE1("File Name: %s\n", buf);
      NTFS_TRACE4("File Permission: %s\t%c%c%c\n",
                  IsDirectory() ? "Directory" : "File",
                  IsReadOnly() ? 'R' : ' ', IsHidden() ? 'H' : ' ',
                  IsSystem() ? 'S' : ' ');
    }
    else
    {
      NTFS_TRACE("Unrecognized File Name or Filename buffer too small\n");
    }
  }

  return len;
}

// Get Unicode File Name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
int Filename::GetFilename(wchar_t* buf, DWORD bufLen) const
{
  if (filename_ == nullptr) return 0;

  if (filename_->name_length)
  {
    if (bufLen < filename_->name_length)
      return -1 * filename_->name_length;  // buffer too small

    bufLen = filename_->name_length;
    wcsncpy(buf, (wchar_t*)filename_->Name, bufLen);
    buf[bufLen] = wchar_t('\0');

    return bufLen;
  }

  return 0;
}

bool Filename::HasName() const { return filename_length_ > 0; }

bool Filename::IsWin32Name() const
{
  if (filename_ == nullptr || filename_length_ <= 0) return FALSE;

  // POSIX, WIN32, WIN32_DOS
  return filename_->NameSpace != Flag::FilenameNamespace::DOS;
}

// Change from UTC time to local time
void Filename::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                           FILETIME* accessTm) const noexcept
{
  if (writeTm)
    AttrStdInfo::UTC2Local(filename_ ? filename_->AlterTime : 0, *writeTm);

  if (createTm)
    AttrStdInfo::UTC2Local(filename_ ? filename_->CreateTime : 0, *createTm);

  if (accessTm)
    AttrStdInfo::UTC2Local(filename_ ? filename_->ReadTime : 0, *accessTm);
}

}  // namespace NtfsBrowser