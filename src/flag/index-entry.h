#pragma once

#include <windows.h>
#include <winnt.h>

namespace NtfsBrowser::Flag
{

enum class IndexEntry : BYTE
{
  SUBNODE = 0x01,  // Index entry points to a sub-node
  LAST = 0x02      // Last index entry in the node, no Stream

};

DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Flag::IndexEntry)

}  // namespace NtfsBrowser::Flag