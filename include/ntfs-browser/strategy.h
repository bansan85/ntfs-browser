#pragma once

#include <cstdint>

namespace NtfsBrowser::Cache {

enum class Strategy : std::uint8_t { NoCache, FullCache };

}  // namespace NtfsBrowser::Cache
