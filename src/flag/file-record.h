#pragma once

#include <winnt.h>

// OK

namespace NtfsBrowser::Flag
{

enum class FileRecord : WORD
{
  INUSE = 0x01,
  DIR = 0x02
};

//NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Flag::FileRecord)

}  // namespace NtfsBrowser::Flag