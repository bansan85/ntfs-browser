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

// Packs (record_ref, attr_type) into a single key for attrListChain.
// record_ref comes from MftSegmentReference::segment_number, a 48-bit
// bitfield, so it fits entirely in the high 48 bits of a ULONGLONG; every
// defined AttrType (data/attr-type.h) is a small multiple of 0x10 no
// larger than LOGGED_UTILITY_STREAM (0x100), comfortably within the low 16
// bits, and IsValidAttrType() has already rejected AttrType::ALL by the
// time this is called - so the two combine below with no realistic
// collision.
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

  // Mark this record's own $ATTRIBUTE_LIST as resolved along this chain
  // before following any of its entries. attrListChain::insert() is a
  // no-op if the key is already present (eg. for an extension record: the
  // entry that led here already inserted this very key just below, before
  // recursing into this record) - the point is the *top-level* record's
  // own key, which nothing else ever inserts, since it is the origin of
  // the chain rather than a target reached through one of its entries.
  // Without this, an entry - anywhere in this chain, however many
  // extension records deep - naming this record again with attr_type
  // ATTRIBUTE_LIST (the only attr_type an entry can carry that makes
  // AllocAttr() recurse into another AttrList, see AllocAttr()'s
  // ATTRIBUTE_LIST case) would go undetected as a cycle back to here.
  attrListChain.insert(
      MakeChainKey(*fr.file_reference_, AttrType::ATTRIBUTE_LIST));

  while ((len = this->ReadData(offset, {reinterpret_cast<BYTE*>(&al_record),
                                        sizeof(Attr::AttributeList)})))
  {
    // A short (but non-empty) read means fewer than sizeof(Attr::AttributeList)
    // bytes remained at this offset - the attribute's real data size is not
    // an exact multiple of the entry size (a malformed/corrupt
    // $ATTRIBUTE_LIST no valid NTFS volume would produce). al_record is
    // reused across iterations rather than reset each time, so treating a
    // short read as a full entry here would parse it with some fields fresh
    // off disk and the rest stale from the previous iteration (see bug F10,
    // docs/bug-reports/2026-09-03-full-repo.md) - stop cleanly instead.
    if (*len != sizeof(Attr::AttributeList))
    {
      NTFS_TRACE2(
          "Attribute List: ReadData returned %I64u bytes, expected %I64u - "
          "stopping\n",
          *len, static_cast<ULONGLONG>(sizeof(Attr::AttributeList)));
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
        // This (record, attribute type) pair was already resolved earlier
        // in this Attribute List chain: extension records referencing each
        // other in a cycle, a duplicate entry, or simply a second entry
        // for a type already retrieved - skip it instead of parsing it
        // again, which would recurse without bound. Note this deliberately
        // does NOT skip a *different* attr_type for the very same
        // record_ref: one extension record commonly hosts several
        // relocated attributes of different types (eg. $INDEX_ROOT and
        // $INDEX_ALLOCATION), and each needs its own resolution.
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