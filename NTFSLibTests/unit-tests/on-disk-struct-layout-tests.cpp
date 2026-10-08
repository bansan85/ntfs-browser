#include <catch2/catch_test_macros.hpp>

#include "attr/attribute-list.h"

TEST_CASE(
    "MftSegmentReference is 8 bytes, matching the real on-disk base file "
    "reference field",
    "[attr-list][regression]") {
  CHECK(sizeof(NtfsBrowser::Attr::MftSegmentReference) == 8);
}
