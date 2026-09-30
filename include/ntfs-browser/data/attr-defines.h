#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{
struct AttrHeaderCommon;

// User defined Callback routines to process raw attribute data
// Set bDiscard to true if this Attribute is to be discarded
// Set bDiscard to false to let FileRecord process it
using AttrRawCallback = void (*)(const AttrHeaderCommon& attrHead,
                                 bool& bDiscard);
}  // namespace NtfsBrowser
