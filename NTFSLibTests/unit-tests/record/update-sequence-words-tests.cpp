#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "record/header.h"

namespace Record = NTFS_BROWSER_NAMESPACE::Record;

TEST_CASE("update sequence words keep what is added, in order",
          "[record][update-sequence]") {
  Record::UpdateSequenceWords words;
  CHECK(words.size() == 0);
  CHECK(words.begin() == words.end());

  words.push_back(0x1111);
  words.push_back(0x2222);
  words.push_back(0x3333);

  REQUIRE(words.size() == 3);
  CHECK(words[0] == 0x1111);
  CHECK(words[1] == 0x2222);
  CHECK(words[2] == 0x3333);
  const std::vector<WORD> seen(words.begin(), words.end());
  CHECK(seen == std::vector<WORD>{0x1111, 0x2222, 0x3333});
}

TEST_CASE(
    "update sequence words hold a sector for each 512 bytes of the "
    "largest record",
    "[record][update-sequence]") {
  Record::UpdateSequenceWords words;
  for (size_t i = 0; i < Record::UpdateSequenceWords::max_words; i++) {
    words.push_back(static_cast<WORD>(i + 1));
  }

  CHECK(Record::UpdateSequenceWords::max_words == 8);
  REQUIRE(words.size() == Record::UpdateSequenceWords::max_words);
  WORD sum = 0;
  for (const WORD word : words) {
    sum = static_cast<WORD>(sum + word);
  }
  CHECK(sum == 36);
}
