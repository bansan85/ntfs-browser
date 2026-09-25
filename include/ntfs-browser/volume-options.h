#pragma once

namespace NtfsBrowser
{

// Volume-wide tolerance flags. Fixed for a NtfsVolume's lifetime (an
// optional constructor argument), read back through GetOptions(). Every
// component that needs one reads it through the owning volume, rather than
// taking its own copy or a per-call override.
struct VolumeOptions
{
  // Off: a freed file record's header still parses (IsDeleted() works), but
  // ParseAttrs() exposes none of its content. On: a freed record's
  // attributes parse normally, as does an extension record reached through
  // one's own $ATTRIBUTE_LIST. Either way, the volume's own metadata reads
  // ($Volume, $MFT, its extension records) bypass this gate, so a freed one
  // doesn't take the whole volume down.
  bool include_deleted{false};

  // Off: a damaged item (a file record, an index block, a data run, an
  // $ATTRIBUTE_LIST) is rejected whole, and the enclosing scan or walk moves
  // on to the next item - no partial content is exposed. On: today's salvage
  // behaviour runs, keeping whatever parsed before the damage.
  bool recover_errors{false};
};

}  // namespace NtfsBrowser
