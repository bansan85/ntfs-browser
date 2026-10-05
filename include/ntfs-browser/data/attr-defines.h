#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{
struct AttrHeaderCommon;

// User defined Callback routines to process raw attribute data
// Set discard to true if this Attribute is to be discarded
// Set discard to false to let FileRecord process it
using AttrRawCallback = void (*)(const AttrHeaderCommon& attr_head,
                                 bool& discard);
}  // namespace NtfsBrowser
