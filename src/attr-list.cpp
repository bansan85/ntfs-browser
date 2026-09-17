#include <stdexcept>

#include "attr-list.h"
#include "attr-non-resident.h"
#include "attr-resident.h"
#include "attr/attribute-list.h"
#include "data/run-entry.h"
#include "ntfs-common.h"
#include <ntfs-browser/mask.h>

namespace NtfsBrowser
{

namespace
{

// Packs (record_ref, attr_type) into one key. record_ref fits the high 48
// bits (MftSegmentReference::segment_number is 48-bit); every AttrType
// fits the low 16 bits.
ULONGLONG MakeChainKey(ULONGLONG recordRef, AttrType attrType) noexcept
{
  return (recordRef << 16) | (static_cast<ULONGLONG>(attrType) & 0xFFFFU);
}

}  // namespace

template <typename TYPE_RESIDENT, Strategy S>
AttrList<TYPE_RESIDENT, S>::AttrList(
    const AttrHeaderCommon& ahc, FileRecord<S>& fr,
    std::unordered_set<ULONGLONG>& attrListChain)
    : TYPE_RESIDENT(ahc, fr)
{
  NTFS_TRACE("Attribute: Attribute List\n");
  if (!fr.file_reference_)
  {
    throw std::runtime_error("Missing file reference\n");
  }

  ULONGLONG offset = 0;
  std::optional<ULONGLONG> len = 0;
  Attr::AttributeList al_record{};

  // Marks this record's own chain key first, so a cycle back to it is caught.
  attrListChain.insert(
      MakeChainKey(*fr.file_reference_, AttrType::ATTRIBUTE_LIST));

  while ((len = this->ReadData(offset, {reinterpret_cast<BYTE*>(&al_record),
                                        Attr::kAttributeListEntryHeaderSize})))
  {
    if (*len != Attr::kAttributeListEntryHeaderSize)
    {
      NTFS_TRACE2(
          "Attribute List: ReadData returned %I64u bytes, expected %I64u - "
          "stopping\n",
          *len, static_cast<ULONGLONG>(Attr::kAttributeListEntryHeaderSize));
      break;
    }

    if (!IsValidAttrType(al_record.attr_type))
    {
      throw std::runtime_error(
          "Attribute List parse error (al_record.attr_type).\n");
    }

    NTFS_TRACE1("Attribute List: 0x%04x\n", al_record.attr_type);

    const ULONGLONG record_ref = al_record.base_ref.segment_number;
    const Mask am = ATTR_MASK(al_record.attr_type);
    // Skip contained attributes
    // Skip unwanted attributes
    if (record_ref != *fr.file_reference_ &&
        static_cast<bool>(am & fr.attr_mask_))
    {
      if (!attrListChain.insert(MakeChainKey(record_ref, al_record.attr_type))
               .second)
      {
        NTFS_TRACE2(
            "Attribute List: record %I64u, type 0x%04x already resolved in "
            "this chain, skipping\n",
            record_ref, al_record.attr_type);
      }
      else
      {
        file_record_list_.emplace_back(fr.volume_);
        FileRecord<S>& frnew = file_record_list_.back();

        frnew.attr_mask_ = am;
        if (!frnew.ParseFileRecord(record_ref))
        {
          throw std::runtime_error(
              "Attribute List parse error (ParseFileRecord).\n");
        }
        if (!frnew.ParseAttrs(attrListChain))
        {
          throw std::runtime_error(
              "Attribute List parse error (ParseAttrs).\n");
        }

        // Insert new found AttrList to fr.AttrList
        std::vector<std::unique_ptr<AttrBase<S>>>& vec =
            frnew.getAttr(al_record.attr_type);
        for (std::unique_ptr<AttrBase<S>>& veci : vec)
        {
          fr.attr_list_[ATTR_INDEX(al_record.attr_type)].push_back(
              std::move(veci));
        }
        vec.clear();
      }
    }

    if (al_record.record_size == 0)
    {
      throw std::runtime_error(
          "Attribute List with zero record size has endless loop.\n");
    }
    offset += al_record.record_size;
  }
}

template <typename TYPE_RESIDENT, Strategy S>
AttrList<TYPE_RESIDENT, S>::~AttrList()
{
  NTFS_TRACE("AttrList deleted\n");
}

template class AttrList<AttrNonResident<Strategy::FULL_CACHE>,
                        Strategy::FULL_CACHE>;
template class AttrList<AttrNonResident<Strategy::NO_CACHE>,
                        Strategy::NO_CACHE>;
template class AttrList<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrList<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser