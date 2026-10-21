#include <ntfs-browser/win-types.h>

#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "data/file-record-header.h"

using NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader;

TEST_CASE("The update sequence array covers no more than it declares",
          "[data][update-sequence]") {
  // A runtime value, so the call is not folded away.
  const size_t record_bytes = 1024;

  SECTION("a declared size of 0 covers nothing") {
    const WORD no_array = 0;
    CHECK(FileRecordHeader::UpdateSequenceBlockCount(record_bytes, no_array) ==
          0);
  }
  SECTION("the sequence number itself covers no block") {
    const WORD only_number = 1;
    CHECK(FileRecordHeader::UpdateSequenceBlockCount(record_bytes,
                                                     only_number) == 0);
  }
  SECTION("a declared array covers its blocks") {
    const WORD two_blocks = 3;
    CHECK(FileRecordHeader::UpdateSequenceBlockCount(record_bytes,
                                                     two_blocks) == 2);
  }
  SECTION("a forged size cannot cover blocks the buffer lacks") {
    const WORD forged = 0xFFFF;
    CHECK(FileRecordHeader::UpdateSequenceBlockCount(record_bytes, forged) ==
          2);
  }
}
