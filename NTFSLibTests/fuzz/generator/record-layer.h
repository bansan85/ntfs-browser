#pragma once

#include <vector>

#include <ntfs-browser/namespace.h>

#include "layer-line.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The file records of a volume that Windows formatted, from named fields. The
// layout (update sequence array, attribute headers, padding, end marker, used
// size) comes from LegacyRecord. The line gives the values.
class RecordLayer {
 public:
  // One attribute of a record: its line, and the entry lines that follow it
  // when it is an $INDEX_ROOT.
  struct Attribute {
    LayerLine line;
    std::vector<LayerLine> entries;
  };

  // The bytes of record number first + offset, from the line "record". The
  // line gives number (the first record), flags, seq (a number, or "@number"
  // for the record number itself), links, lsn, usn, next_id and base, all
  // numbers that default to zero, and pad (the four bytes after the end
  // marker). count and the address are for the caller. The attributes follow in
  // order. Their lines are:
  //   si, fn         a $STANDARD_INFORMATION and a $FILE_NAME, with the fields
  //                  of AttributeValues
  //   indexroot      an $INDEX_ROOT (see IndexLayer::Root), named by aname
  //   attr           any resident attribute: type and value (hexadecimal)
  //   nonres         any non-resident attribute: type, start, last, alloc,
  //                  real, init, unit, csize and runs (hexadecimal)
  // Every attribute takes id, flags, its name (aname) and noff, the name offset
  // it declares when that is not the usual one. A resident one takes indexed.
  [[nodiscard]] static std::vector<BYTE>
      Render(const LayerLine& line, const std::vector<Attribute>& attributes,
             ULONGLONG offset);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
