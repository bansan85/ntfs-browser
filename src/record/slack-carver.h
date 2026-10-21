#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/record/carved-name.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Record {

// Looks for $FILE_NAME structures in the slack of a file record: the bytes
// after the attribute end marker, up to the end of the record. record is the
// whole record, update sequence fixups already removed. marker_offset is where
// the 4-byte end marker (0xFFFFFFFF) sits in it.
//
// NTFS writes four more bytes after the marker, and the record's used size
// ends after them. Those eight bytes belong to the live record. The scan
// starts after the marker word, since a leftover structure can begin inside
// the four bytes that follow it, but it trusts nothing before marker_offset +
// 8: a field that starts there is reported as unknown (parent_known or
// times_known false) and never read.
//
// The scan steps 2 bytes at a time, the alignment of a UTF-16 name. It looks
// for the length byte, the namespace byte and the characters of a name, then
// checks the fixed fields before them that the slack still holds: a complete
// candidate has a parent reference that is not record 0, four times between
// 1980 and 2100, and attribute flags NTFS knows. A candidate whose start was
// overwritten is kept when its name has 5 characters or more, and is marked
// partial. Each candidate is reported once, whatever the number of copies.
// Every byte read is inside record, and every step moves forward.
//
// The times of a result are as stored, in UTC: the caller converts them.
// Candidates come in the order they sit in the record.
NTFS_BROWSER_EXPORT_TESTS_ONLY std::vector<CarvedName>
    CarveFileNames(std::span<const BYTE> record, size_t marker_offset);

}  // namespace NTFS_BROWSER_NAMESPACE::Record
