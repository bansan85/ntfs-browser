#pragma once

#include <cstdint>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

// Volume-wide tolerance flags. Fixed for a NtfsVolume's lifetime (an
// optional constructor argument), read back through GetOptions(). Every
// component that needs one reads it through the owning volume, rather than
// taking its own copy or a per-call override.
struct VolumeOptions {
  // Off: a freed file record's header still parses (IsDeleted() works), but
  // ParseAttrs() exposes none of its content. Directory entries that name a
  // freed record, or one reused under another sequence number, are hidden
  // from listings, name lookups and walks. On: a freed record's attributes
  // parse normally, as does an extension record reached through one's own
  // $ATTRIBUTE_LIST, and those entries show. Either way, the volume's own
  // metadata reads ($Volume, $MFT, its extension records) bypass this gate,
  // so a freed one doesn't take the whole volume down.
  bool include_deleted{false};

  // Off: a damaged item (a file record, an index block, a cluster run, an
  // $ATTRIBUTE_LIST) is rejected whole, and the enclosing scan or walk moves
  // on to the next item - no partial content is exposed. On: today's salvage
  // behaviour runs, keeping whatever parsed before the damage. A record that
  // an index entry names, but that does not parse, is a damaged item too:
  // hidden when off, returned when on, with its metadata taken from that
  // entry. On, a walk also reads the $INDEX_ALLOCATION blocks no index
  // pointer reaches, up to max_orphan_scan_blocks; an entry found that way is
  // kept only if its parent matches.
  bool recover_errors{false};

  // Soft ceiling, per volume, on the memory FullCache holds. 256 MiB by
  // default. NoCache ignores it. After each read, the cache frees the fewest
  // unused blocks needed to get back under it, least recently used first.
  // Memory that a live Cache::View holds is never freed, so the total can stay
  // above the ceiling while such views exist. Cache::unlimited_bytes removes
  // the limit. 0 frees a block as soon as its last view is dropped. Last
  // member, so designated initialisers that stop before it stay valid.
  std::uint64_t max_cache_bytes{Cache::default_max_cache_bytes};
};

}  // namespace NTFS_BROWSER_NAMESPACE
