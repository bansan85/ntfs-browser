#include "attr-vol-name.h"
#include "ntfs-common.h"

// OK

namespace NtfsBrowser
{

AttrVolName::AttrVolName(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr)
{
  NTFS_TRACE("Attribute: Volume Name\n");

  name_.resize((GetDataSize() / 2) + 1, '\0');
  memcpy(name_.data(), GetData(), GetDataSize());
}

// Get NTFS Volume Unicode Name
std::wstring_view AttrVolName::GetName() const noexcept { return name_; }

}  // namespace NtfsBrowser
