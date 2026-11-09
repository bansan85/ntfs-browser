#pragma once

#include <cstdint>
#include <span>
#include <unordered_set>

#include <ntfs-browser/namespace.h>

#include "attr/type.h"
#include "io/file-record-impl.h"
#include "io/file-record.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Io {

// Resolves the entries of an $ATTRIBUTE_LIST: opens the extension record an
// entry names and moves the attributes it holds into the Impl of the record
// that owns the list. Short-lived: built over an Impl for the resolution of one
// list, never stored.
template <Cache::Strategy S>
class FileRecord<S>::Impl::ExtensionImporter {
 public:
  // impl MUST outlive this object. attr_list_chain carries the (record,
  // attribute type) pairs this resolution already visited. When recovering, an
  // entry that names a foreign record is skipped, else it throws.
  ExtensionImporter(Impl& impl, std::unordered_set<ULONGLONG>& attr_list_chain,
                    bool recover) noexcept;

  // Imports the attributes of type attr_type from extension record
  // record_number. sequence_number is the one the entry recorded for it: a
  // record that another file reused since is skipped. Throws when strict and
  // the record cannot be used.
  void Import(ULONGLONG record_number, WORD sequence_number,
              Attr::Type attr_type);

 private:
  // An extension record that an $ATTRIBUTE_LIST entry names, and what the
  // entry asks of it. claim_only: the type is listed, not parsed.
  struct Request {
    ULONGLONG record_number;
    Attr::Type attr_type;
    bool claim_only;
  };

  // Whether the caller read slot record_number and saw that it names a base
  // record other than the listing one.
  [[nodiscard]] bool NamesOtherBase(ULONGLONG record_number,
                                    ULONGLONG listing_record) const;

  // Handles a record an entry names that is not an extension of this one.
  // Throws when strict and this record is in use.
  void SkipForeign(ULONGLONG record_number);

  // Decides whether the record the entry names is to be opened at all, and
  // sets held to the bytes the caller already has for it (empty: read it).
  [[nodiscard]] bool Wants(const Request& request, std::span<const BYTE>& held);

  // Opens the extension record of an entry and parses its header. Null when a
  // claim-only record cannot be parsed.
  [[nodiscard]] Io::FileRecord<S>* Open(const Request& request,
                                        std::span<const BYTE> held);

  // Whether extension is still the extension record the entry of sequence
  // number sequence_number meant, and not a slot another file reused.
  [[nodiscard]] bool IsGenuine(const Io::FileRecord<S>& extension,
                               WORD sequence_number) const;

  // The Impl of the record that owns the list. It MUST outlive this object.
  Impl& impl_;
  std::unordered_set<ULONGLONG>& attr_list_chain_;
  bool recover_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
