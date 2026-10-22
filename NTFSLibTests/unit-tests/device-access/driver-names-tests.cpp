#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "driver-names.h"

using NtfsDevice::DecodeDriverName;
using NtfsDevice::IsDriverMappingKnown;
using NtfsDevice::NeedsDriverMapping;

namespace {

// A wide string holding the single code unit unit, which may be a surrogate.
std::wstring Unit(unsigned unit) {
  return std::wstring(1, static_cast<wchar_t>(unit));
}

}  // namespace

TEST_CASE("NeedsDriverMapping is true only for an unpaired surrogate",
          "[device-access][driver-names]") {
  CHECK(NeedsDriverMapping(Unit(0xD800)));
  CHECK(NeedsDriverMapping(L"a" + Unit(0xDC80) + L"b"));
  CHECK_FALSE(NeedsDriverMapping(L"plain"));
  CHECK_FALSE(NeedsDriverMapping(L"café"));
  CHECK_FALSE(NeedsDriverMapping(L"\U0001F600"));
  CHECK_FALSE(NeedsDriverMapping(L""));
}

TEST_CASE("The ntfs-3g mount types are a known mapping",
          "[device-access][driver-names]") {
  CHECK(IsDriverMappingKnown("fuse"));
  CHECK(IsDriverMappingKnown("fuseblk"));
}

TEST_CASE("The ntfs3 driver has no mapping until it is measured",
          "[device-access][driver-names]") {
  CHECK_FALSE(IsDriverMappingKnown("ntfs3"));
  CHECK_FALSE(DecodeDriverName("ntfs3", "\xED\xB2\x80").has_value());
}

TEST_CASE("ntfs-3g renders a lone surrogate in WTF-8 and it decodes back",
          "[device-access][driver-names]") {
  const auto high = DecodeDriverName("fuse", "\xED\xA0\x80");
  const auto low = DecodeDriverName("fuse",
                                    "\xED\xB2\x80"
                                    "a");

  REQUIRE(high.has_value());
  CHECK(*high == Unit(0xD800));
  REQUIRE(low.has_value());
  CHECK(*low == Unit(0xDC80) + L"a");
}

TEST_CASE("A name with no mapping need decodes unchanged",
          "[device-access][driver-names]") {
  const auto plain = DecodeDriverName("fuseblk", "caf\xC3\xA9");

  REQUIRE(plain.has_value());
  CHECK(*plain == L"café");
}

TEST_CASE("An unknown filesystem type gives no decoding",
          "[device-access][driver-names]") {
  CHECK_FALSE(IsDriverMappingKnown("ext4"));
  CHECK_FALSE(IsDriverMappingKnown(""));
  CHECK_FALSE(DecodeDriverName("ext4", "plain").has_value());
}
