#include "corpus-test-support.h"

#include <catch2/catch_test_macros.hpp>

namespace NtfsBrowserTests
{

void RequireCorpusImage(const std::filesystem::path& image)
{
  if (std::filesystem::exists(image))
  {
    return;
  }
#ifdef NTFS_TEST_REQUIRE_DATA
  FAIL("test image not present: " << image.string());
#else
  SKIP("test image not present: " << image.string());
#endif
}

}  // namespace NtfsBrowserTests
