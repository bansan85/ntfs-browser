#pragma once

namespace NtfsBrowser
{

enum class AttrFlag
{
  COMPRESSED = 0x0001,
  ENCRYPTED = 0x4000,
  SPARSE = 0x8000
};

}  // namespace NtfsBrowser