#include "attr/filename.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>

#include <ntfs-browser/filename.h>
#include <ntfs-browser/log.h>
#include <ntfs-browser/strategy.h>

#include "attr-resident.h"
#include "attr-std-info.h"
#include "flag/filename-namespace.h"
#include "flag/filename.h"
#include "mft-file-reference.h"
#include "ntfs-common.h"
#include "utf.h"

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
  LogTrace("Filename Copied");

  filename_ = &afn;
  filename_wuc_ = fn.filename_wuc_;
}

// Decodes the file name and caches it in filename_wuc_, for Compare().
void Filename::GetFilenameWUC() { (void)GetFilename(); }

// Compare Unicode file name
int Filename::Compare(std::wstring_view fn) const noexcept
{
  // Only the overlap is safe: the on-disk name isn't null-terminated. Folds
  // to uppercase, matching NTFS' real $I30 collation order, not the
  // platform CRT's lowercase-folding comparison.
  const size_t n = std::min(fn.size(), filename_wuc_.size());
  for (size_t i = 0; i < n; ++i)
  {
    const wint_t ca = std::towupper(static_cast<wint_t>(fn[i]));
    const wint_t cb = std::towupper(static_cast<wint_t>(filename_wuc_[i]));
    if (ca != cb)
    {
      return ca < cb ? -1 : 1;
    }
    if (fn[i] == L'\0')
    {
      break;
    }
  }
  if (fn.size() == filename_wuc_.size())
  {
    return 0;
  }
  return fn.size() < filename_wuc_.size() ? -1 : 1;
}

ULONGLONG Filename::GetFileSize() const noexcept
{
  return filename_ != nullptr ? filename_->real_size : 0;
}

ULONGLONG Filename::GetAllocatedSize() const noexcept
{
  return filename_ != nullptr ? filename_->alloc_size : 0;
}

ULONGLONG Filename::GetParentReference() const noexcept
{
  return filename_ != nullptr ? filename_->parent_ref & kMftRecordNumberMask
                              : 0;
}

WORD Filename::GetParentSequenceNumber() const noexcept
{
  return filename_ != nullptr
             ? static_cast<WORD>(filename_->parent_ref >> kMftSequenceShift)
             : 0;
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

bool Filename::IsArchive() const noexcept
{
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::ARCHIVE)
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

  // filename_->name is raw on-disk UTF-16 (WORD, always 16 bits), not
  // wchar_t (16 bits on Windows, but wider elsewhere): decode rather than
  // reinterpret_cast, so a name outside the BMP survives intact everywhere.
  filename_wuc_ = Utf16ToWide(std::u16string_view(
      reinterpret_cast<const char16_t*>(&filename_->name[0]),
      filename_->name_length));
  const std::wstring_view retval = filename_wuc_;

  // Guarded: this runs once per directory entry, and the UTF-8 conversion
  // below allocates whether or not anything would print it.
  if (!retval.empty() && IsLogged(Log::Level::kDebug))
  {
    LogDebug("File Name: {}", WideToUtf8(retval));
    LogDebug("File Permission: {}\t{}{}{}",
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
                           FILETIME* accessTm,
                           FILETIME* changeTm) const noexcept
{
  if (writeTm != nullptr)
  {
    AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>::UTC2Local(
        filename_ != nullptr ? filename_->alter_time : 0, *writeTm);
  }

  if (createTm != nullptr)
  {
    AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>::UTC2Local(
        filename_ != nullptr ? filename_->create_time : 0, *createTm);
  }

  if (accessTm != nullptr)
  {
    AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>::UTC2Local(
        filename_ != nullptr ? filename_->read_time : 0, *accessTm);
  }

  if (changeTm != nullptr)
  {
    AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>::UTC2Local(
        filename_ != nullptr ? filename_->mft_time : 0, *changeTm);
  }
}

}  // namespace NtfsBrowser