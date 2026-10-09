#include "attr/filename.h"

#include <ntfs-browser/win-types.h>

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
#include "upcase.h"
#include "utf.h"

namespace NtfsBrowser {

void Filename::SetFilename(const Attr::Filename& filename) {
  filename_ = &filename;

  GetFilenameWUC();
}

// Copy pointer buffers
void Filename::CopyFilename(const Filename& filename,
                            const Attr::Filename& afn) {
  Log::Trace("Filename Copied");

  filename_ = &afn;
  filename_wuc_ = filename.filename_wuc_;
}

// Decodes the file name and caches it in filename_wuc_, for Compare().
void Filename::GetFilenameWUC() const { (void)GetFilename(); }

int Filename::Compare(std::wstring_view file_name) const noexcept {
  return Compare(file_name, UpCaseTable::BuiltIn());
}

// Only the decoded name is compared: the on-disk one isn't null-terminated.
int Filename::Compare(std::wstring_view file_name,
                      const UpCaseTable& upcase) const noexcept {
  return upcase.Compare(file_name, filename_wuc_);
}

ULONGLONG Filename::GetFileSize() const noexcept {
  return filename_ != nullptr ? filename_->real_size : 0;
}

ULONGLONG Filename::GetAllocatedSize() const noexcept {
  return filename_ != nullptr ? filename_->alloc_size : 0;
}

ULONGLONG Filename::GetParentReference() const noexcept {
  return filename_ != nullptr
             ? filename_->parent_ref & Mft::mft_record_number_mask
             : 0;
}

WORD Filename::GetParentSequenceNumber() const noexcept {
  return filename_ != nullptr ? static_cast<WORD>(filename_->parent_ref >>
                                                  Mft::mft_sequence_shift)
                              : 0;
}

Flag::Filename Filename::GetFilePermission() const noexcept {
  return filename_ != nullptr ? filename_->flags : Flag::Filename::None;
}

bool Filename::IsReadOnly() const noexcept {
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::ReadOnly)
             : false;
}

bool Filename::IsHidden() const noexcept {
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::Hidden)
             : false;
}

bool Filename::IsSystem() const noexcept {
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::System)
             : false;
}

bool Filename::IsArchive() const noexcept {
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::Archive)
             : false;
}

bool Filename::IsDirectory() const noexcept {
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::Directory)
             : false;
}

bool Filename::IsCompressed() const noexcept {
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::Compressed)
             : false;
}

bool Filename::IsEncrypted() const noexcept {
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::Encrypted)
             : false;
}

bool Filename::IsSparse() const noexcept {
  return filename_ != nullptr
             ? static_cast<bool>(filename_->flags & Flag::Filename::Sparse)
             : false;
}

// Get Unicode File Name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
std::wstring_view Filename::GetFilename() const {
  if (filename_ == nullptr) {
    return {};
  }

  // filename_->name is raw on-disk UTF-16 (WORD, always 16 bits), not
  // wchar_t (16 bits on Windows, but wider elsewhere): decode rather than
  // reinterpret_cast, so a name outside the BMP survives intact everywhere.
  filename_wuc_ = Utf::Utf16ToWide(std::u16string_view(
      reinterpret_cast<const char16_t*>(&filename_->name[0]),
      filename_->name_length));
  const std::wstring_view retval = filename_wuc_;

  // Guarded: this runs once per directory entry, and the UTF-8 conversion
  // below allocates whether or not anything would print it.
  if (!retval.empty() && Log::IsLogged(Log::Level::Debug)) {
    Log::Debug("File Name: {}", Utf::WideToUtf8(retval));
    Log::Debug("File Permission: {}\t{}{}{}",
               IsDirectory() ? "Directory" : "File", IsReadOnly() ? 'R' : ' ',
               IsHidden() ? 'H' : ' ', IsSystem() ? 'S' : ' ');
  }

  return retval;
}

bool Filename::HasName() const noexcept { return !filename_wuc_.empty(); }

bool Filename::IsWin32Name() const noexcept {
  if (filename_ == nullptr || filename_wuc_.empty()) {
    return false;
  }

  // POSIX, WIN32, WIN32_DOS
  return filename_->name_space != Flag::FilenameNamespace::Dos;
}

// Change from UTC time to local time
void Filename::GetFileTime(FILETIME* write_tm, FILETIME* create_tm,
                           FILETIME* access_tm,
                           FILETIME* change_tm) const noexcept {
  if (write_tm != nullptr) {
    AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>::UTC2Local(
        filename_ != nullptr ? filename_->alter_time : 0, *write_tm);
  }

  if (create_tm != nullptr) {
    AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>::UTC2Local(
        filename_ != nullptr ? filename_->create_time : 0, *create_tm);
  }

  if (access_tm != nullptr) {
    AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>::UTC2Local(
        filename_ != nullptr ? filename_->read_time : 0, *access_tm);
  }

  if (change_tm != nullptr) {
    AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>::UTC2Local(
        filename_ != nullptr ? filename_->mft_time : 0, *change_tm);
  }
}

}  // namespace NtfsBrowser
