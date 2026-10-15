#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "data/attribute-list.h"

TEST_CASE(
    "MftSegmentReference is 8 bytes, matching the real on-disk base file "
    "reference field",
    "[attr-list][regression]") {
  CHECK(sizeof(NTFS_BROWSER_NAMESPACE::Data::MftSegmentReference) == 8);
}
