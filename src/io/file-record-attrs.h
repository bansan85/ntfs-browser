#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

#include "attr/resident.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Io {

template <Cache::Strategy S>
class AttrParser;

// How the walk over a record's attribute headers ended.
enum class WalkEnd : BYTE {
  EndMarker,  // The terminating Attr::Type::All marker was reached.
  RanOut,     // The record ended without a marker, or an attribute overran it.
  Aborted     // An attribute was malformed, or the visit stopped the walk.
};

// The resident base the attribute wrappers of strategy S are built over.
template <Cache::Strategy S>
using ResidentOf =
    std::conditional_t<S == Cache::Strategy::NoCache, Attr::AttrResidentNoCache,
                       Attr::AttrResidentFullCache>;

// Parses the attributes of one file record into its Impl: walks the attribute
// headers, wraps each wanted attribute, and merges the VCN-split ones.
// Short-lived: built over an Impl for one ParseAttrs() call, never stored.
template <Cache::Strategy S>
class AttrParser {
 public:
  explicit AttrParser(typename FileRecord<S>::Impl& impl) noexcept;

  // Clears the previous attributes, then parses every wanted one of the record
  // into the Impl. attr_list_chain carries one $ATTRIBUTE_LIST resolution's
  // already-visited (record, attribute type) pairs into this record's own
  // attribute parse, instead of starting a fresh chain.
  [[nodiscard]] bool Parse(std::unordered_set<ULONGLONG>& attr_list_chain);

 private:
  // The Impl whose attributes this fills. It MUST outlive this object.
  typename FileRecord<S>::Impl& impl_;

  [[nodiscard]] const Attr::HeaderCommon&
      AlignedAttrHeader(std::span<const BYTE> bytes);
  [[nodiscard]] std::unique_ptr<Attr::AttrBase<S>>
      AllocAttr(const Attr::HeaderCommon& ahc, bool& unhandled,
                std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] bool ParseAttr(const Attr::HeaderCommon& ahc,
                               std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] const Attr::HeaderCommon* FirstAttrToWalk();
  [[nodiscard]] WalkEnd
      WalkAttrHeaders(const Attr::HeaderCommon& first,
                      std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] bool VisitAttr(std::span<const BYTE> cur,
                               const Attr::HeaderCommon& head,
                               std::unordered_set<ULONGLONG>& attr_list_chain);
  void MergeAttributeContinuations();
  static void
      MergeStreamChain(std::vector<std::unique_ptr<Attr::AttrBase<S>>>& attrs,
                       std::vector<size_t>& indices,
                       std::vector<size_t>& to_erase);
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
