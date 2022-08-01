#include <crtdbg.h>

#include <ntfs-browser/ntfs-common.h>
#include <ntfs-browser/file-name.h>
#include <ntfs-browser/data/attr-filename-flag.h>
#include <ntfs-browser/data/attr-filename-namespace.h>
#include <ntfs-browser/attr-std-info.h>

namespace NtfsBrowser
{

FileName::FileName(Data::AttrFilename* fn)
{
  IsCopy = FALSE;

  filename_ = fn;

  FileNameWUC = NULL;
  FileNameLength = 0;

  if (fn) GetFileNameWUC();
}

FileName::~FileName()
{
  if (FileNameWUC) delete FileNameWUC;
}

void FileName::SetFileName(Data::AttrFilename* fn)
{
  filename_ = fn;

  GetFileNameWUC();
}

// Copy pointer buffers
void FileName::CopyFileName(const FileName* fn, const Data::AttrFilename* afn)
{
  if (!IsCopy)
  {
    NTFS_TRACE("Cannot call this routine\n");
    return;
  }

  _ASSERT(fn && afn);

  NTFS_TRACE("FileName Copied\n");

  if (FileNameWUC)
  {
    delete FileNameWUC;
    FileNameWUC = NULL;
  }

  FileNameLength = fn->FileNameLength;
  filename_ = afn;

  if (fn->FileNameWUC)
  {
    FileNameWUC = new wchar_t[FileNameLength + 1];
    wcsncpy(FileNameWUC, fn->FileNameWUC, FileNameLength);
    FileNameWUC[FileNameLength] = wchar_t('\0');
  }
  else
    FileNameWUC = NULL;
}

// Get uppercase unicode filename and store it in a buffer
void FileName::GetFileNameWUC()
{
#ifdef _DEBUG
  char fna[MAX_PATH];
  GetFileName(fna, MAX_PATH);  // Just show filename in debug window
#endif

  if (FileNameWUC)
  {
    delete FileNameWUC;
    FileNameWUC = NULL;
    FileNameLength = 0;
  }

  wchar_t fns[MAX_PATH];
  FileNameLength = GetFileName(fns, MAX_PATH);

  if (FileNameLength > 0)
  {
    FileNameWUC = new wchar_t[FileNameLength + 1];
    for (int i = 0; i < FileNameLength; i++) FileNameWUC[i] = towupper(fns[i]);
    FileNameWUC[FileNameLength] = wchar_t('\0');
  }
  else
  {
    FileNameLength = 0;
    FileNameWUC = NULL;
  }
}

// Compare Unicode file name
int FileName::Compare(const wchar_t* fn) const
{
  // Change fn to upper case
  size_t len = wcslen(fn);
  if (len > MAX_PATH) return 1;  // Assume bigger

  wchar_t fns[MAX_PATH];

  for (int i = 0; i < len; i++) fns[i] = towupper(fn[i]);
  fns[len] = wchar_t('\0');

  return wcscmp(fns, FileNameWUC);
}

// Compare ANSI file name
int FileName::Compare(const char* fn) const
{
  wchar_t fnw[MAX_PATH];

  int len = MultiByteToWideChar(CP_ACP, 0, fn, -1, fnw, MAX_PATH);
  if (len)
    return Compare(fnw);
  else
    return 1;  // Assume bigger
}

ULONGLONG FileName::GetFileSize() const
{
  return filename_ ? filename_->RealSize : 0;
}

DWORD FileName::GetFilePermission() const
{
  return filename_ ? filename_->Flags : 0;
}

BOOL FileName::IsReadOnly() const
{
  return filename_ ? ((filename_->Flags) &
                      static_cast<DWORD>(AttrFilenameFlag::READONLY))
                   : FALSE;
}

BOOL FileName::IsHidden() const
{
  return filename_ ? ((filename_->Flags) &
                      static_cast<DWORD>(AttrFilenameFlag::HIDDEN))
                   : FALSE;
}

BOOL FileName::IsSystem() const
{
  return filename_ ? ((filename_->Flags) &
                      static_cast<DWORD>(AttrFilenameFlag::SYSTEM))
                   : FALSE;
}

BOOL FileName::IsDirectory() const
{
  return filename_ ? ((filename_->Flags) &
                      static_cast<DWORD>(AttrFilenameFlag::DIRECTORY))
                   : FALSE;
}

BOOL FileName::IsCompressed() const
{
  return filename_ ? ((filename_->Flags) &
                      static_cast<DWORD>(AttrFilenameFlag::COMPRESSED))
                   : FALSE;
}

BOOL FileName::IsEncrypted() const
{
  return filename_ ? ((filename_->Flags) &
                      static_cast<DWORD>(AttrFilenameFlag::ENCRYPTED))
                   : FALSE;
}

BOOL FileName::IsSparse() const
{
  return filename_ ? ((filename_->Flags) &
                      static_cast<DWORD>(AttrFilenameFlag::SPARSE))
                   : FALSE;
}

// Get ANSI File Name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
int FileName::GetFileName(char* buf, DWORD bufLen) const
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
      NTFS_TRACE("Unrecognized File Name or FileName buffer too small\n");
    }
  }

  return len;
}

// Get Unicode File Name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
int FileName::GetFileName(wchar_t* buf, DWORD bufLen) const
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

BOOL FileName::HasName() const { return FileNameLength > 0; }

BOOL FileName::IsWin32Name() const
{
  if (filename_ == NULL || FileNameLength <= 0) return FALSE;

  return (filename_->NameSpace !=
          static_cast<BYTE>(
              AttrFilenameNamespace::DOS));  // POSIX, WIN32, WIN32_DOS
}

// Change from UTC time to local time
void FileName::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                           FILETIME* accessTm) const
{
  AttrStdInfo::UTC2Local(filename_ ? filename_->AlterTime : 0, writeTm);

  if (createTm)
    AttrStdInfo::UTC2Local(filename_ ? filename_->CreateTime : 0, createTm);

  if (accessTm)
    AttrStdInfo::UTC2Local(filename_ ? filename_->ReadTime : 0, accessTm);
}

}  // namespace NtfsBrowser