#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Attr {

struct HeaderCommon;

// User defined Callback routines to process raw attribute data
// Set discard to true if this Attribute is to be discarded
// Set discard to false to let FileRecord process it
using RawCallback = void (*)(const HeaderCommon& attr_head, bool& discard);

}  // namespace NtfsBrowser::Attr
