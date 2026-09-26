#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/strategy.h>

#include "file-reader.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileReader;
using NtfsBrowser::Strategy;

TEST_CASE(
    "FileReader<FULL_CACHE>::Read must not abort/misbehave on a read "
    "crossing a 64KiB cache block boundary",
    "[file-reader][regression]")
{
  // Each byte distinct from its offset, so returned data can be checked
  // exactly.
  std::vector<BYTE> backing(2 * 65536);
  for (size_t i = 0; i < backing.size(); i++)
  {
    backing[i] = static_cast<BYTE>(i);
  }

  auto reader_double =
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing);
  FileReader<Strategy::FULL_CACHE> reader(std::move(reader_double));

  // Straddles the boundary between the first and second 64KiB cache blocks.
  LARGE_INTEGER addr{.QuadPart = 65536 - 36};
  constexpr DWORD kLength = 100;

  // A crossing read may abort the process; ctest reports that as this
  // test failing.
  const std::optional<std::span<const BYTE>> result =
      reader.Read(addr, kLength);

  if (result)
  {
    CHECK(result->size() == kLength);
    for (size_t i = 0; i < result->size(); i++)
    {
      CHECK((*result)[i] == backing[65536 - 36 + i]);
    }
  }
}
