#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Attr {

struct HeaderCommon;

// User defined Callback routines to process raw attribute data
// Set discard to true if this Attribute is to be discarded
// Set discard to false to let FileRecord process it
using RawCallback = void (*)(const HeaderCommon& attr_head, bool& discard);

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
