#include "attr-vol-name.h"
#include "ntfs-common.h"

// OK

namespace NtfsBrowser
{

AttrVolName::AttrVolName(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr)
{
  NTFS_TRACE("Attribute: Volume Name\n");

  name_.resize((attr_body_size_ / 2) + 1, '\0');
  memcpy(name_.data(), attr_body_, attr_body_size_);
}

// Get NTFS Volume Unicode Name
std::wstring_view AttrVolName::GetName() const { return name_; }

}  // namespace NtfsBrowser
