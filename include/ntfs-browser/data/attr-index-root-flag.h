#pragma once

namespace NtfsBrowser
{

enum class AttrIndexRootFlag
{
  SMALL = 0x00,  // Fits in Index Root File Record
  LARGE = 0x01   // Index Allocation and Bitmap needed

};

}  // namespace NtfsBrowser