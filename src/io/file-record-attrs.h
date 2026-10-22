#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

#include "io/file-record-attr-types.h"
#include "io/file-record-impl.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

struct HeaderCommon;

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Io {

// Parses the attributes of one file record into its Impl: walks the attribute
// headers, wraps each wanted attribute, and merges the VCN-split ones.
// Short-lived: built over an Impl for one ParseAttrs() call, never stored.
template <Cache::Strategy S>
class FileRecord<S>::Impl::AttrParser {
 public:
  explicit AttrParser(Impl& impl) noexcept;

  // Clears the previous attributes, then parses every wanted one of the record
  // into the Impl. attr_list_chain carries one $ATTRIBUTE_LIST resolution's
  // already-visited (record, attribute type) pairs into this record's own
  // attribute parse, instead of starting a fresh chain.
  [[nodiscard]] bool Parse(std::unordered_set<ULONGLONG>& attr_list_chain);

 private:
  // The Impl whose attributes this fills. It MUST outlive this object.
  Impl& impl_;
  // Set by the walk when recovering skipped an attribute that failed to parse.
  bool skipped_attr_{false};

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

}  // namespace NTFS_BROWSER_NAMESPACE::Io
