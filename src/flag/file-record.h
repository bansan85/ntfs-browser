#pragma once

#include <winnt.h>

namespace NtfsBrowser::Flag
{

enum class FileRecord : WORD
{
  INUSE = 0x01,
  DIR = 0x02
};

DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Flag::FileRecord)

}  // namespace NtfsBrowser::Flag