#pragma once

#include <ntfs-browser/win-types.h>

namespace {

// Sentinel fill so untouched bytes are distinguishable from valid (zero) data.
constexpr BYTE sentinel_byte = 0xCC;

}  // namespace
