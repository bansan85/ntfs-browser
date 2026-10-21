#include <ntfs-browser/win-types.h>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "mft/file-reference.h"
#include "mft/record-offset.h"

namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

TEST_CASE("NextSequence wraps past the largest sequence to 1, never 0",
          "[mft][file-reference]") {
  const WORD largest = Mft::mft_max_sequence;
  const WORD middle = 7;

  CHECK(Mft::NextSequence(largest) == 1);
  CHECK(Mft::NextSequence(middle) == 8);
}

TEST_CASE("a freed record is the same generation as the one before its wrap",
          "[mft][file-reference]") {
  const WORD largest = Mft::mft_max_sequence;
  const WORD wrapped = 1;
  const WORD other = 5;

  CHECK(Mft::IsSameRecordGeneration(largest, wrapped, false));
  CHECK_FALSE(Mft::IsSameRecordGeneration(largest, wrapped, true));
  CHECK_FALSE(Mft::IsSameRecordGeneration(largest, other, false));
  CHECK(Mft::IsSameRecordGeneration(0, other, true));
}

TEST_CASE("an extension record needs a matching sequence and the listing base",
          "[mft][file-reference]") {
  const ULONGLONG base = 42;
  const ULONGLONG base_with_sequence =
      base | (ULONGLONG{9} << Mft::mft_sequence_shift);

  CHECK(Mft::IsGenuineExtensionRecord(0, 3, base_with_sequence, base));
  CHECK(Mft::IsGenuineExtensionRecord(3, 3, base, base));
  CHECK_FALSE(Mft::IsGenuineExtensionRecord(4, 3, base, base));
  CHECK_FALSE(Mft::IsGenuineExtensionRecord(3, 3, base + 1, base));
}

TEST_CASE("RecordOffset gives no offset for a record size of 0 or an overflow",
          "[mft][file-reference]") {
  const DWORD record_size = 1024;
  const DWORD no_record_size = 0;
  const ULONGLONG record = 7;
  const ULONGLONG huge_record = ~ULONGLONG{0};

  CHECK(Mft::RecordOffset(record_size, record) == 7 * 1024);
  CHECK_FALSE(Mft::RecordOffset(no_record_size, record));
  CHECK_FALSE(Mft::RecordOffset(record_size, huge_record));
}
