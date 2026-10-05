#include "attr-vol-name.h"

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/strategy.h>

#include "attr-resident.h"
#include "ntfs-common.h"
#include "utf.h"

namespace NtfsBrowser
{

template <typename Resident, Strategy S>
AttrVolName<Resident, S>::AttrVolName(const AttrHeaderCommon& ahc,
                                      const FileRecord<S>& file_record)
    : Resident(ahc, file_record)
{
  LogTrace("Attribute: Volume Name");

  // The volume name is raw on-disk UTF-16 (WORD, always 16 bits), not
  // wchar_t (16 bits on Windows, but wider elsewhere): decode rather than
  // copy its bytes directly into name_'s own.
  name_ = Utf16ToWide(
      std::u16string_view(reinterpret_cast<const char16_t*>(this->GetData()),
                          this->GetDataSize() / sizeof(WORD)));
  // A trailing NUL GetName()'s view still covers; TrimTrailingNuls()
  // (ntfs-volume.cpp) strips it before anything logs or compares the name.
  name_.push_back(L'\0');
}

// Get NTFS Volume Unicode Name
template <typename Resident, Strategy S>
std::wstring_view AttrVolName<Resident, S>::GetName() const noexcept
{
  return name_;
}

template class AttrVolName<AttrResidentFullCache, Strategy::FullCache>;
template class AttrVolName<AttrResidentNoCache, Strategy::NoCache>;

}  // namespace NtfsBrowser
