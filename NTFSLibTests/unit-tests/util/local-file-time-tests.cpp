#include <ntfs-browser/win-types.h>

#include <cstdint>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>

#include "util/local-file-time.h"

namespace Util = NTFS_BROWSER_NAMESPACE::Util;

TEST_CASE("a time converted to local gives its UTC ticks back",
          "[util][local-file-time]") {
  const ULONGLONG utc = GENERATE(ULONGLONG{1}, ULONGLONG{0x0102030405060708},
                                 ULONGLONG{133'497'936'000'000'000},
                                 ULONGLONG{0x3132333435363738});
  FILETIME local{};

  Util::UtcToLocalFileTime(utc, local);

  CHECK(Util::LocalToUtcTicks(local) == utc);
}

TEST_CASE("a local time of zero stays zero", "[util][local-file-time]") {
  CHECK(Util::LocalToUtcTicks(FILETIME{}) == 0);
}
