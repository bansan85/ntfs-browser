#pragma once

#include <windows.h>

namespace NtfsBrowser
{
struct AttrHeaderCommon;
constexpr size_t kAttrNums = 16;  // Attribute Types count

// User defined Callback routines to process raw attribute data
// Set bDiscard to true if this Attribute is to be discarded
// Set bDiscard to false to let FileRecord process it
using AttrRawCallback = void (*)(const AttrHeaderCommon& attrHead,
                                 bool& bDiscard);
}  // namespace NtfsBrowser
