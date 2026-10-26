#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/record/carved-name.h>

#include "attr/slot.h"
#include "io/file-record-attr-types.h"
#include "record/header.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

enum class Type : DWORD;
struct HeaderCommon;
template <Cache::Strategy S>
class AttrBase;
template <typename Resident, Cache::Strategy S>
class AttrStdInfo;

}  // namespace Attr

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache
template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

namespace Io {

// Everything FileRecord<S> keeps out of its public header: the members, and the
// private methods that work on them. Only FileRecord<S> reaches it, through
// impl_: other classes use the methods of FileRecord<S> that forward to it.
// AttrParser, nested here, fills the attribute lists it holds.
// file-record.cpp instantiates this class, but ParseAttrs() and AttrParser are
// defined in file-record-attrs.cpp: MSVC then warns (C4661) at the class line.
#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4661)
#endif
template <Cache::Strategy S>
class FileRecord<S>::Impl {
 public:
  Impl(Io::FileRecord<S>& self, const NtfsVolume<S>& volume) noexcept;

  // Defined in file-record-attrs.h, see AttrParser::Parse().
  class AttrParser;

  // The FileRecord this belongs to. Attributes are built over it, and private
  // methods that need a public one go through it. A pointer, not a reference:
  // FileRecord's move constructor MUST repoint it.
  Io::FileRecord<S>* self;
  const NtfsVolume<S>* volume;
  std::unique_ptr<Record::HeaderImpl<S>> file_record;
  std::optional<ULONGLONG> file_reference;
  std::array<Attr::RawCallback, Attr::attr_nums> attr_raw_call_back{};
  Attr::Mask attr_mask{Attr::Mask::All};
  // The types, besides those of attr_mask, whose non-resident attributes the
  // parse only lists in claimed_layouts: no attribute is built for them, so a
  // defect in one changes nothing else about the parse. See SetClaimMask().
  Attr::Mask claim_mask{static_cast<Attr::Mask>(0)};
  // The layouts of the non-resident attributes of claim_mask, from this record
  // and from the extension records its $ATTRIBUTE_LIST names.
  std::vector<Alloc::StreamLayout> claimed_layouts;

  // The extension records $ATTRIBUTE_LIST opened. An attribute imported from
  // one keeps a reference into its bytes, so they MUST outlive attr_list_:
  // declared first, destroyed last, and cleared after the attributes.
  // Unlike std::vector, appending never moves existing elements' addresses.
  std::list<Io::FileRecord<S>> extension_records;
  // Aligned copies of the attributes that sit at a misaligned address in
  // record_buffer_. A parsed attribute keeps a reference into its copy, so
  // these MUST outlive attr_list_: declared before it, cleared after it.
  std::vector<std::vector<BYTE>> realigned_attrs;
  std::array<std::vector<std::unique_ptr<Attr::AttrBase<S>>>, Attr::attr_nums>
      attr_list{};

  // False makes AttrParser::AllocAttr() wrap $ATTRIBUTE_LIST generically, not
  // via AttrList.
  bool resolve_attr_list{true};

  // Set by the caller that refuses the volume reads of a parse, and told when
  // one is refused. Null: the parse reads. See SetReadRefusal().
  bool* read_refusal{nullptr};

  // True bypasses the deleted-record gate in ParseAttrs(): set by
  // NtfsVolume on its own internal FileRecords ($Volume, $MFT, $MFT
  // extension records), so a freed one doesn't take the whole volume down,
  // whatever include_deleted says.
  bool bypass_deleted_gate{false};

  // Owned per-instance so this FileRecord's raw bytes (viewed by NoCache
  // attributes as plain pointers/spans, no copy) are never aliased by
  // another FileRecord's read (eg. NtfsVolume's $MFT record vs. this one).
  std::vector<BYTE> record_buffer;

  // The $ATTRIBUTE_LIST chain of the running ParseAttrs(). Kept from one call
  // to the next, emptied each time, so that a parse does not allocate a set.
  std::unordered_set<ULONGLONG> attr_list_chain;

  // Where ImportAttrListEntry() looks for an extension record before it reads
  // it from the disk. Empty: always from the disk.
  typename Io::FileRecord<S>::RecordSource extension_source;
  // Which base record a slot names, for a slot extension_source does not hold.
  // Empty: every such slot is read from the disk.
  typename Io::FileRecord<S>::BaseSource base_source;

  // Where the attribute walk of the last ParseAttrs() met the end marker.
  // Empty when it did not reach one.
  std::optional<size_t> end_marker_offset;

  // The update sequence check of the last ParseFileRecord() failed on at least
  // one sector, whether the record was then rejected or kept.
  bool failed_fixup{false};
  // The last ParseFileRecord() kept a record whose update sequence check
  // failed, or read it from $MFTMirr instead. Only when recovering.
  bool header_salvaged{false};
  // The last ParseAttrs() salvaged something strict mode rejects. Only when
  // recovering.
  bool attrs_salvaged{false};
  // Extension records of a freed listing that another file reused, skipped by
  // the last ParseAttrs(). Not a defect, so not part of attrs_salvaged. A
  // record that several entries name is kept once. A vector: it is nearly
  // always empty, and an empty one allocates nothing.
  std::vector<ULONGLONG> reused_extensions;
  // The last ParseAttrs() dropped an overwritten $ATTRIBUTE_LIST of a freed
  // record, see FileRecord<S>::HasOverwrittenAttrList(). Not a defect, so not
  // part of attrs_salvaged.
  bool overwritten_list{false};

  void ClearAttrs() noexcept;
  // Lists the layout of the non-resident attribute at ahc in claimed_layouts.
  // An attribute that cannot be decoded is left out.
  void ClaimLayout(const Attr::HeaderCommon& ahc);
  // See ImportAttrListEntry(): whether the caller read slot record_number and
  // saw that it names a base record other than the listing one.
  [[nodiscard]] bool NamesOtherBase(ULONGLONG record_number,
                                    ULONGLONG listing_record) const;
  // Handles a record an $ATTRIBUTE_LIST entry names that is not an extension
  // of this one. Throws when strict and this record is in use.
  void SkipForeignExtension(ULONGLONG record_number, bool recover);
  // See FileRecord<S>::CarveSlackNames().
  [[nodiscard]] std::vector<Record::CarvedName> CarveSlackNames() const;
  // Turns the UTC times of carved, as the carvers report them, into those of
  // GetFileTime(). Unknown times stay zero.
  static void ConvertCarvedTimes(std::vector<Record::CarvedName>& carved);
  // Stays here: it reaches the volume's private Impl, which only
  // FileRecord<S> may name. Attributes call it through this Impl.
  void UserCallBack(DWORD att_type, const Attr::HeaderCommon& ahc,
                    bool& discard);
  // See FileRecord<S>::ImportAttrListEntry().
  void ImportAttrListEntry(ULONGLONG record_number, WORD sequence_number,
                           Attr::Type attr_type,
                           std::unordered_set<ULONGLONG>& attr_list_chain,
                           bool recover);
  // Parses the attributes of this record, see AttrParser::Parse(). Defined in
  // file-record-attrs.cpp.
  [[nodiscard]] bool ParseAttrs(std::unordered_set<ULONGLONG>& attr_list_chain);
  // Reads file_ref at its fixed offset from mft_addr, see ReadFileRecord().
  [[nodiscard]] bool ReadDirectRecord(ULONGLONG file_ref);
  // preread is the bytes of record file_ref when the caller already has them,
  // empty to read them. See ParseRecord().
  [[nodiscard]] std::unique_ptr<Record::HeaderImpl<S>>
      ReadFileRecord(ULONGLONG file_ref, std::span<const BYTE> preread);
  // The header of record_buffer, see ReadFileRecord() and ReadMirrorRecord().
  [[nodiscard]] std::unique_ptr<Record::HeaderImpl<S>> ParseHeader();
  // Reads the $MFTMirr copy of file_ref into record_buffer. Null when there is
  // no copy to read, or it has no usable header.
  [[nodiscard]] std::unique_ptr<Record::HeaderImpl<S>>
      ReadMirrorRecord(ULONGLONG file_ref);
  // ParseFileRecord() with the record's bytes optionally supplied by the
  // caller: they are copied, so they may be reused right after.
  [[nodiscard]] bool ParseRecord(ULONGLONG file_ref,
                                 std::span<const BYTE> preread);
  [[nodiscard]] const Attr::AttrStdInfo<ResidentOf<S>, S>*
      StdInfo() const noexcept;
  [[nodiscard]] bool
      StdInfoFlag(bool (Attr::AttrStdInfo<ResidentOf<S>, S>::*flag)()
                      const noexcept) const noexcept;
};
#ifdef _MSC_VER
  #pragma warning(pop)
#endif

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
