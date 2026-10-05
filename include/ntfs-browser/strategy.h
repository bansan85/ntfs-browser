#pragma once

#include <cstdint>

namespace NtfsBrowser
{

enum class Strategy : std::uint8_t
{
  NoCache,
  FullCache
};

}  // namespace NtfsBrowser
