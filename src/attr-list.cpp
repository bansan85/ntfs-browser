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

template <typename TYPE_RESIDENT, Strategy S>
AttrList<TYPE_RESIDENT, S>::AttrList(const AttrHeaderCommon& ahc,
                                     FileRecord<S>& fr)
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

  // Lazily start (or inherit, if fr is itself an extension record opened by
  // an outer AttrList) the set of file references resolved along this
  // Attribute List chain, so a record that loops back to one already seen
  // (self-reference or a cycle between extension records) is caught below
  // instead of being parsed again forever.
  if (!fr.attr_list_chain_)
  {
    fr.attr_list_chain_ = std::make_shared<std::unordered_set<ULONGLONG>>();
    fr.attr_list_chain_->insert(*fr.file_reference_);
  }

  while ((len = this->ReadData(offset, {reinterpret_cast<BYTE*>(&al_record),
                                        sizeof(Attr::AttributeList)})) &&
         *len == sizeof(Attr::AttributeList))
  {
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
      if (!fr.attr_list_chain_->insert(record_ref).second)
      {
        // record_ref was already resolved earlier in this Attribute List
        // chain (extension records referencing each other in a cycle, or
        // an entry naming the same record twice) - skip it instead of
        // parsing it again, which would recurse without bound.
        NTFS_TRACE1(
            "Attribute List: record %I64u already resolved in this chain, "
            "skipping\n",
            record_ref);
      }
      else
      {
        file_record_list_.emplace_back(fr.volume_);
        FileRecord<S>& frnew = file_record_list_.back();

        frnew.attr_mask_ = am;
        frnew.attr_list_chain_ = fr.attr_list_chain_;
        if (!frnew.ParseFileRecord(record_ref))
        {
          throw std::runtime_error(
              "Attribute List parse error (ParseFileRecord).\n");
        }
        if (!frnew.ParseAttrs())
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