#include "attr-file-name.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <stdexcept>

#include <ntfs-browser/strategy.h>

#include "attr-resident.h"
#include "attr/filename.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
template <Strategy S>
class FileRecord;

template <typename RESIDENT, Strategy S>
AttrFileName<RESIDENT, S>::AttrFileName(const AttrHeaderCommon& ahc,
                                        const FileRecord<S>& file_record)
    : RESIDENT(ahc, file_record)
{
  LogTrace("Attribute: File Name");

  if (this->GetDataSize() < offsetof(Attr::Filename, name))
  {
    throw std::runtime_error("File Name attribute smaller than expected.\n");
  }

  const auto& filename =
      *reinterpret_cast<const Attr::Filename*>(this->GetData());
  // Attribute size MUST cover fixed header and name data.
  if (this->GetDataSize() <
      offsetof(Attr::Filename, name) +
          (static_cast<ULONGLONG>(filename.name_length) * sizeof(WORD)))
  {
    throw std::runtime_error(
        "File Name attribute name exceeds attribute bounds.\n");
  }

  SetFilename(filename);
}

template <typename RESIDENT, Strategy S>
AttrFileName<RESIDENT, S>::~AttrFileName()
{
  LogTrace("AttrFileName deleted");
}

template class AttrFileName<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser
