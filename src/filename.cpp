#include <crtdbg.h>

#include "ntfs-common.h"
#include <ntfs-browser/filename.h>
#include "attr/filename.h"
#include "flag/filename.h"
#include "flag/filename-namespace.h"
#include "attr-std-info.h"

namespace NtfsBrowser
{

Filename::Filename() { filename_ = nullptr; }

void Filename::SetFilename(const Attr::Filename* fn)
{
  filename_ = fn;

  GetFilenameWUC();
}

// Copy pointer buffers
void Filename::CopyFilename(const Filename* fn, const Attr::Filename* afn)
{
  _ASSERT(fn && afn);

  NTFS_TRACE("Filename Copied\n");

  filename_ = afn;
  filename_wuc_ = fn->filename_wuc_;
}

// Get uppercase unicode filename and store it in a buffer
void Filename::GetFilenameWUC()
{
  filename_wuc_ = GetFilename();
  std::transform(filename_wuc_.begin(), filename_wuc_.end(),
                 filename_wuc_.begin(), ::towupper);
}

// Compare Unicode file name
int Filename::Compare(std::wstring_view fn) const
{
  return wcsicmp(fn.data(), filename_wuc_.c_str());
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

// Get Unicode File Name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
std::wstring Filename::GetFilename() const
{
  if (filename_ == nullptr) return {};

  std::wstring retval;
  retval.resize(filename_->name_length);
  retval.assign((const wchar_t*)filename_->Name, filename_->name_length);

  if (!retval.empty())
  {
    NTFS_TRACE1("File Name: %s\n", retval.c_str());
    NTFS_TRACE4("File Permission: %s\t%c%c%c\n",
                IsDirectory() ? "Directory" : "File", IsReadOnly() ? 'R' : ' ',
                IsHidden() ? 'H' : ' ', IsSystem() ? 'S' : ' ');
  }

  return retval;
}

bool Filename::HasName() const { return filename_wuc_.length(); }

bool Filename::IsWin32Name() const
{
  if (filename_ == nullptr || filename_wuc_.empty()) return FALSE;

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