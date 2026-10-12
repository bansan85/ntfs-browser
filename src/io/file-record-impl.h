#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <list>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>

#include "attr/slot.h"
#include "data/file-record-header.h"
#include "record/header.h"

namespace NtfsBrowser {

namespace Io {

// Everything FileRecord<S> keeps out of its public header: the members, and the
// private methods that work on them. Friends of FileRecord<S> (AttrList,
// NtfsVolume) reach it through FileRecord<S>::impl_. AttrParser<S> fills the
// attribute lists it holds.
template <Cache::Strategy S>
class FileRecord<S>::Impl {
 public:
  Impl(Io::FileRecord<S>& self, const NtfsVolume<S>& volume) noexcept;

  // The FileRecord this belongs to. Attributes are built over it, and private
  // methods that need a public one go through it. A pointer, not a reference:
  // FileRecord's move constructor MUST repoint it.
  Io::FileRecord<S>* self;
  const NtfsVolume<S>* volume;
  std::unique_ptr<Record::HeaderImpl<S>> file_record;
  std::optional<ULONGLONG> file_reference;
  std::array<Attr::RawCallback, Attr::attr_nums> attr_raw_call_back{};
  Attr::Mask attr_mask{Attr::Mask::All};

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

  // True bypasses the deleted-record gate in ParseAttrs(): set by
  // NtfsVolume on its own internal FileRecords ($Volume, $MFT, $MFT
  // extension records), so a freed one doesn't take the whole volume down,
  // whatever include_deleted says.
  bool bypass_deleted_gate{false};

  // Owned per-instance so this FileRecord's raw bytes (viewed by NoCache
  // attributes as plain pointers/spans, no copy) are never aliased by
  // another FileRecord's read (eg. NtfsVolume's $MFT record vs. this one).
  std::vector<BYTE> record_buffer;

  void ClearAttrs() noexcept;
  // Stays here: it reaches the volume's private Impl, which only
  // FileRecord<S> may name. Attributes call it through this Impl.
  void UserCallBack(DWORD att_type, const Attr::HeaderCommon& ahc,
                    bool& discard);
  // Parses the attributes of this record, see AttrParser<S>::Parse().
  [[nodiscard]] bool ParseAttrs(std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] std::unique_ptr<Record::HeaderImpl<S>>
      ReadFileRecord(ULONGLONG file_ref);
};

}  // namespace Io

}  // namespace NtfsBrowser
