#pragma once

namespace NtfsBrowser
{

enum class IndexEntryFlag
{
  SUBNODE = 0x01,  // Index entry points to a sub-node
  LAST = 0x02      // Last index entry in the node, no Stream

};

}  // namespace NtfsBrowser