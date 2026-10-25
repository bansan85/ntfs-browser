#pragma once

#include <cstdint>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

// How the file record that a directory entry names stands, as the probe of a
// directory walk found it.
enum class RecordStanding : std::uint8_t {
  // The record is in use, and carries the sequence number the entry names (or
  // the entry names none).
  Live,
  // The record is freed: the file is deleted.
  Deleted,
  // The record is in use under another sequence number. Another file took it
  // since: the entry is stale, and the record is not this entry's file.
  Reused,
  // The record does not parse. Only a recovering volume reports such an entry.
  Unreadable
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
