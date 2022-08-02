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
  IsCopy = FALSE;

  filename_ = NULL;

  FilenameWUC = NULL;
  FilenameLength = 0;
}

Filename::~Filename()
{
  if (FilenameWUC) delete FilenameWUC;
}

void Filename::SetFilename(Attr::Filename* fn)
{
  filename_ = fn;

  GetFilenameWUC();
}

// Copy pointer buffers
void Filename::CopyFilename(const Filename* fn, const Attr::Filename* afn)
{
  if (!IsCopy)
  {
    NTFS_TRACE("Cannot call this routine\n");
    return;
  }

  _ASSERT(fn && afn);

  NTFS_TRACE("Filename Copied\n");

  if (FilenameWUC)
  {
    delete FilenameWUC;
    FilenameWUC = NULL;
  }

  FilenameLength = fn->FilenameLength;
  filename_ = afn;

  if (fn->FilenameWUC)
  {
    FilenameWUC = new wchar_t[FilenameLength + 1];
    wcsncpy(FilenameWUC, fn->FilenameWUC, FilenameLength);
    FilenameWUC[FilenameLength] = wchar_t('\0');
  }
  else
    FilenameWUC = NULL;
}

// Get uppercase unicode filename and store it in a buffer
void Filename::GetFilenameWUC()
{
#ifdef _DEBUG
  char fna[MAX_PATH];
  GetFilename(fna, MAX_PATH);  // Just show filename in debug window
#endif

  if (FilenameWUC)
  {
    delete FilenameWUC;
    FilenameWUC = NULL;
    FilenameLength = 0;
  }

  wchar_t fns[MAX_PATH];
  FilenameLength = GetFilename(fns, MAX_PATH);

  if (FilenameLength > 0)
  {
    FilenameWUC = new wchar_t[FilenameLength + 1];
    for (int i = 0; i < FilenameLength; i++) FilenameWUC[i] = towupper(fns[i]);
    FilenameWUC[FilenameLength] = wchar_t('\0');
  }
  else
  {
    FilenameLength = 0;
    FilenameWUC = NULL;
  }
}

// Compare Unicode file name
int Filename::Compare(const wchar_t* fn) const
{
  // Change fn to upper case
  size_t len = wcslen(fn);
  if (len > MAX_PATH) return 1;  // Assume bigger

  wchar_t fns[MAX_PATH];

  for (int i = 0; i < len; i++) fns[i] = towupper(fn[i]);
  fns[len] = wchar_t('\0');

  return wcscmp(fns, FilenameWUC);
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

Flag::Filename Filename::GetFilePermission() const
{
  return filename_ ? filename_->Flags : Flag::Filename::NONE;
}

BOOL Filename::IsReadOnly() const
{
  return filename_
             ? static_cast<BOOL>(filename_->Flags & Flag::Filename::READONLY)
             : FALSE;
}

BOOL Filename::IsHidden() const
{
  return filename_
             ? static_cast<BOOL>(filename_->Flags & Flag::Filename::HIDDEN)
             : FALSE;
}

BOOL Filename::IsSystem() const
{
  return filename_
             ? static_cast<BOOL>(filename_->Flags & Flag::Filename::SYSTEM)
             : FALSE;
}

BOOL Filename::IsDirectory() const
{
  return filename_
             ? static_cast<BOOL>(filename_->Flags & Flag::Filename::DIRECTORY)
             : FALSE;
}

BOOL Filename::IsCompressed() const
{
  return filename_
             ? static_cast<BOOL>(filename_->Flags & Flag::Filename::COMPRESSED)
             : FALSE;
}

BOOL Filename::IsEncrypted() const
{
  return filename_
             ? static_cast<BOOL>(filename_->Flags & Flag::Filename::ENCRYPTED)
             : FALSE;
}

BOOL Filename::IsSparse() const
{
  return filename_
             ? static_cast<BOOL>(filename_->Flags & Flag::Filename::SPARSE)
             : FALSE;
}

// Get ANSI File Name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
int Filename::GetFilename(char* buf, DWORD bufLen) const
{
  if (filename_ == NULL) return 0;

  int len = 0;

  if (filename_->NameLength)
  {
    if (bufLen < filename_->NameLength)
      return -1 * filename_->NameLength;  // buffer too small

    len = WideCharToMultiByte(CP_ACP, 0, (wchar_t*)filename_->Name,
                              filename_->NameLength, buf, bufLen, NULL, NULL);
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
  if (filename_ == NULL) return 0;

  if (filename_->NameLength)
  {
    if (bufLen < filename_->NameLength)
      return -1 * filename_->NameLength;  // buffer too small

    bufLen = filename_->NameLength;
    wcsncpy(buf, (wchar_t*)filename_->Name, bufLen);
    buf[bufLen] = wchar_t('\0');

    return bufLen;
  }

  return 0;
}

BOOL Filename::HasName() const { return FilenameLength > 0; }

BOOL Filename::IsWin32Name() const
{
  if (filename_ == NULL || FilenameLength <= 0) return FALSE;

  // POSIX, WIN32, WIN32_DOS
  return filename_->NameSpace != Flag::FilenameNamespace::DOS;
}

// Change from UTC time to local time
void Filename::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                           FILETIME* accessTm) const
{
  AttrStdInfo::UTC2Local(filename_ ? filename_->AlterTime : 0, writeTm);

  if (createTm)
    AttrStdInfo::UTC2Local(filename_ ? filename_->CreateTime : 0, createTm);

  if (accessTm)
    AttrStdInfo::UTC2Local(filename_ ? filename_->ReadTime : 0, accessTm);
}

}  // namespace NtfsBrowser