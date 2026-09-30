#pragma once

#include <cstdint>

namespace NtfsBrowser
{

enum class Strategy : std::uint8_t
{
  NO_CACHE,
  FULL_CACHE
};

}  // namespace NtfsBrowser
