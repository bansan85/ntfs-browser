#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>

#include <ntfs-browser/namespace.h>

#include "attr/defines.h"
#include "attr/mask.h"
#include "attr/slot.h"
#include "attr/type.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// The raw bytes of an $MFT record, as $MFT holds them, when the caller
// already has them, else an empty span.
using RecordSource = std::function<std::span<const BYTE>(ULONGLONG record)>;

// The record number of the base record that slot `record` of $MFT names, as
// the caller read it: 0 for a base record. Nothing when the caller did not
// read that slot.
using BaseSource = std::function<std::optional<ULONGLONG>(ULONGLONG record)>;

// The raw bytes of one $INDEX_ALLOCATION block of the directory `directory`,
// at sub-node VCN `vcn`, as the walk of that directory asks for it. Empty
// when the caller does not hold it.
using IndexBlockSource =
    std::function<std::span<const BYTE>(ULONGLONG directory, ULONGLONG vcn)>;

// Takes the records of one run of $MFT that a probe has just read, from the
// slot `first_record` on, `bytes` long. Only valid for the call.
using RunSink =
    std::function<void(ULONGLONG first_record, std::span<const BYTE> bytes)>;

// Called by a probe after it has read one run of $MFT, from the slot
// `first_record` on, `count` records long: the run as it was read, widened
// to clusters when the store keeps runs. Called once for each run, whether
// the read succeeded or not, in the order the runs are read, after the
// records of the run are probed. The run MAY be one that an announcement
// named.
using RunNote = std::function<void(ULONGLONG first_record, ULONGLONG count)>;

// What the probes of FileRecord::TraverseSubEntriesWithStanding() may reuse,
// and where they leave what they read. `find` answers as a RecordSource does,
// an empty `keep` keeps nothing, and an empty `ran` is never called and costs
// nothing. The three work apart: a store MAY set any of them.
struct ProbeStore {
  RecordSource find;
  RunSink keep;
  RunNote ran;
};

// How a FileRecord parses: which attributes it keeps, what it may reuse
// instead of reading the volume, and which reads it refuses. The caller fills
// one and hands it over in a single call, FileRecord::SetParseOptions(). The
// defaults are those of a record that nobody configured: every attribute, no
// source, every read allowed. Every function and flag the options hold MUST
// outlive the parses that use them.
struct RecordParseOptions {
  // The attribute types to parse. Standard information and the attribute list
  // are always added, and so is the $EFS stream when Data is in the mask:
  // unless exact_attr_mask is set. Unwanted attributes are dropped without a
  // word. All: every attribute, as a record that nobody configured.
  Attr::Mask attr_mask{Attr::Mask::All};

  // True keeps exactly attr_mask and adds none of the attributes above. For
  // the volume's own metadata records, whose list is resolved by other code.
  bool exact_attr_mask{false};

  // The types, besides those of attr_mask, whose non-resident attributes the
  // parse lists as layouts, in this record and in the extension records its
  // $ATTRIBUTE_LIST names, without building an attribute for them: no defect
  // in one changes whether the record parses, what it logs at Warn or Error,
  // or what the record keeps. A type already in attr_mask is not listed here:
  // its attributes give their own GetLayout(). The extension records such a
  // type needs are read like the others, and are skipped without a word when
  // one cannot be used. Default: none.
  Attr::Mask claim_mask{static_cast<Attr::Mask>(0)};

  // False makes a $ATTRIBUTE_LIST be wrapped as a plain attribute, not
  // resolved into the attributes it names.
  bool resolve_attr_list{true};

  // True marks the record as volume metadata ($MFT, $UpCase, ...): the parse
  // then ignores the deleted flag, whatever include_deleted says. A directory
  // record that bypasses the gate also shows the entries that name freed
  // records, since volume metadata is read whatever include_deleted says.
  bool bypass_deleted_gate{false};

  // Where the parse takes the extension records its $ATTRIBUTE_LIST names
  // from. A record the source does not give is read from the volume.
  RecordSource extension_source;

  // Which base record the slots of $MFT name, for a slot the extension source
  // does not hold. A listed record that names another base is not an extension
  // of this one, whatever its sequence number: it is skipped like any other
  // foreign record, without a read of the volume. A slot the source knows
  // nothing about, or that names this record, is read as usual. It MUST answer
  // as the volume would: the same bytes were read.
  BaseSource base_source;

  // Where ParseFileRecord(file_ref) takes the bytes of a record that $MFT maps
  // before it reads them from the volume. A record the source does not give is
  // read as usual. The bytes MUST be the ones the volume holds, as
  // ReadMftSlots() gives them. Unlike ParseFileRecord(file_ref, preread), only
  // where the bytes come from changes: a record with a failed update sequence
  // check, or one read from $MFTMirr, goes through the same steps and logs the
  // same lines. A record below Mft::Idx::User is read from its fixed address,
  // and never asked of the source. Not used for the extension records of
  // $ATTRIBUTE_LIST.
  RecordSource record_source;

  // Where the $INDEX_ALLOCATION attribute that a later ParseAttrs() builds
  // takes the bytes of its blocks before it reads them from the volume. A
  // block the source does not give is read as usual, and a block it gives is
  // parsed as the volume's would be. An $INDEX_ALLOCATION that comes from an
  // extension record does not use it. The source is asked under the number
  // given to ParseFileRecord().
  IndexBlockSource index_block_source;

  // Where the probes of TraverseSubEntriesWithStanding() take the records of
  // the entries they judge before they read them from $MFT, and where they
  // leave each run they read, so that the probes of the next directory find
  // it. A record that `find` gives is not read, and its bytes MUST be the
  // ones the volume holds: the walk, its log and its listing do not change,
  // only the reads do. Not used by TraverseSubEntries().
  ProbeStore probe_store;

  // Makes the parses refuse the reads of the volume that a $ATTRIBUTE_LIST or
  // an $EFS stream would cause: an extension record the source does not give,
  // a non-resident list, a non-resident $EFS stream. What would be read is
  // skipped, and *read_refusal is set. A parse that set it is incomplete, and
  // its result MUST be thrown away. For a parse on a thread that MUST NOT
  // touch the volume. Null: the parse reads.
  bool* read_refusal{nullptr};

  // The raw attribute callback of each attribute type, for this record alone,
  // by Attr::AttrIndex(). A null entry has none: the volume's callback then
  // applies. Fill it with SetRawCallback().
  std::array<Attr::RawCallback, Attr::attr_nums> attr_raw_callbacks{};

  // Sets the raw attribute callback of attr_type. False, and nothing set, for
  // a type that has no slot.
  bool SetRawCallback(Attr::Type attr_type,
                      Attr::RawCallback callback) noexcept {
    const DWORD index = Attr::AttrIndex(attr_type);
    if (index >= Attr::attr_nums) {
      return false;
    }
    attr_raw_callbacks.at(index) = callback;
    return true;
  }
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
