#include <crtdbg.h>

#include <ntfs-browser/filename.h>

#include "attr-std-info.h"
#include "attr/filename.h"
#include "flag/filename-namespace.h"
#include "flag/filename.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

void Filename::SetFilename(const Attr::Filename& fn)
{
  filename_ = &fn;

  GetFilenameWUC();
}

// Copy pointer buffers
void Filename::CopyFilename(const Filename& fn, const Attr::Filename& afn)
{
  NTFS_TRACE("Filename Copied\n");

  filename_ = &afn;
  filename_wuc_ = fn.filename_wuc_;
}

// Get uppercase unicode filename and store it in a buffer
void Filename::GetFilenameWUC() { filename_wuc_ = GetFilename(); }

// Compare Unicode file name
int Filename::Compare(std::wstring_view fn) const noexcept
{
  return _wcsicmp(fn.data(), filename_wuc_.data());
}

ULONGLONG Filename::GetFileSize() const noexcept
{
  return filename_ != nullptr ? filename_->real_size : 0;
}

Flag::Filename Filename::GetFilePermission() const noexcept
{
  return filename_ != nullptr ? filename_->flags : Flag::Filename::NONE;
}

bool Filename::IsReadOnly() const noexcept
{
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::READONLY)
             : false;
}

bool Filename::IsHidden() const noexcept
{
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::HIDDEN)
             : false;
}

bool Filename::IsSystem() const noexcept
{
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::SYSTEM)
             : false;
}

bool Filename::IsDirectory() const noexcept
{
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::DIRECTORY)
             : false;
}

bool Filename::IsCompressed() const noexcept
{
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::COMPRESSED)
             : false;
}

bool Filename::IsEncrypted() const noexcept
{
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::ENCRYPTED)
             : false;
}

bool Filename::IsSparse() const noexcept
{
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::SPARSE)
             : false;
}

// Get Unicode File Name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
std::wstring_view Filename::GetFilename() const
{
  if (filename_ == nullptr)
  {
    return {};
  }

  std::wstring_view retval(
      reinterpret_cast<const wchar_t*>(&filename_->name[0]),
      filename_->name_length);

  if (!retval.empty())
  {
    NTFS_TRACE1("File Name: %s\n", retval.c_str());
    NTFS_TRACE4("File Permission: %s\t%c%c%c\n",
                IsDirectory() ? "Directory" : "File", IsReadOnly() ? 'R' : ' ',
                IsHidden() ? 'H' : ' ', IsSystem() ? 'S' : ' ');
  }

  return retval;
}

bool Filename::HasName() const noexcept { return !filename_wuc_.empty(); }

bool Filename::IsWin32Name() const noexcept
{
  if (filename_ == nullptr || filename_wuc_.empty())
  {
    return false;
  }

  // POSIX, WIN32, WIN32_DOS
  return filename_->name_space != Flag::FilenameNamespace::DOS;
}

// Change from UTC time to local time
void Filename::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                           FILETIME* accessTm) const noexcept
{
  if (writeTm != nullptr)
  {
    AttrStdInfo<AttrResidentHeavy>::UTC2Local(
        filename_ != nullptr ? filename_->alter_time : 0, *writeTm);
  }

  if (createTm != nullptr)
  {
    AttrStdInfo<AttrResidentHeavy>::UTC2Local(
        filename_ != nullptr ? filename_->create_time : 0, *createTm);
  }

  if (accessTm != nullptr)
  {
    AttrStdInfo<AttrResidentHeavy>::UTC2Local(
        filename_ != nullptr ? filename_->read_time : 0, *accessTm);
  }
}

}  // namespace NtfsBrowser