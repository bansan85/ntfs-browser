#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/namespace.h>

#include "data/index-entry-flag.h"
#include "data/index-entry.h"

namespace Data = NTFS_BROWSER_NAMESPACE::Data;
using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;

TEST_CASE("An index entry whose stream cannot fit its size has no name",
          "[cov-nri]") {
  std::vector<BYTE> buffer(256);
  auto& entry = *reinterpret_cast<Data::IndexEntry*>(buffer.data());

  SECTION("a stream claimed by an entry with no room past its header") {
    entry.size = static_cast<WORD>(offsetof(Data::IndexEntry, stream));
    entry.stream_size = 4;
    const IndexEntryView view{entry};
    CHECK_FALSE(view.HasName());
  }

  SECTION("a stream too small to hold a file name header") {
    entry.size = static_cast<WORD>(offsetof(Data::IndexEntry, stream) + 4);
    entry.stream_size = 4;
    const IndexEntryView view{entry};
    CHECK_FALSE(view.HasName());
  }

  SECTION("an entry without a stream copies cleanly, with no name") {
    entry.size = static_cast<WORD>(offsetof(Data::IndexEntry, stream));
    entry.stream_size = 0;
    entry.flags = Data::IndexEntryFlag::SubNode;
    const IndexEntry copy{IndexEntryView(entry)};
    CHECK_FALSE(copy.HasName());
    CHECK_FALSE(copy.IsSubNodePtr());
  }
}
