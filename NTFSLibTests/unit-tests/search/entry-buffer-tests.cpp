#include <cstddef>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "search/entry-buffer.h"

namespace Lib = NTFS_BROWSER_NAMESPACE::Search;

namespace {

// A name that differs for each n and is long enough to fill chunks fast.
std::u16string NameOf(std::size_t n) {
  return u"name-" + std::u16string(300, static_cast<char16_t>(u'a' + n % 26)) +
         std::u16string(1, static_cast<char16_t>(0x4E00 + n % 100));
}

}  // namespace

TEST_CASE("An EntryBuffer keeps the text of every entry it holds",
          "[search][buffer]") {
  Lib::EntryBuffer buffer;
  constexpr std::size_t count = 500;

  for (std::size_t n = 0; n < count; ++n) {
    // The text lives in temporaries that die at the end of each pass.
    const std::u16string name = NameOf(n);
    const std::u16string path = u"\\dir\\" + name;
    Lib::Entry entry;
    entry.name = name;
    entry.path = path;
    entry.record = n;
    buffer.Add(entry);
  }

  REQUIRE(buffer.Size() == count);
  for (std::size_t n = 0; n < count; ++n) {
    const Lib::Entry& stored = buffer.Entries()[n];
    CHECK(stored.record == n);
    CHECK(stored.name == NameOf(n));
    CHECK(stored.path == u"\\dir\\" + NameOf(n));
  }
}

TEST_CASE("An EntryBuffer stores a name longer than a chunk",
          "[search][buffer]") {
  Lib::EntryBuffer buffer;
  const std::u16string huge(100000, u'x');
  Lib::Entry entry;
  entry.name = huge;

  buffer.Add(entry);
  buffer.Add(entry);

  CHECK(buffer.Entries()[0].name == huge);
  CHECK(buffer.Entries()[1].name == huge);
}

TEST_CASE("An EntryBuffer copies a cluster summary", "[search][buffer]") {
  Lib::EntryBuffer buffer;
  {
    Lib::ClusterSummary summary;
    summary.clusters.at(
        static_cast<std::size_t>(Lib::ClusterCondition::Intact)) = 9;
    Lib::Entry with;
    with.clusters = &summary;
    buffer.Add(with);
    summary.clusters.fill(0);
  }
  buffer.Add(Lib::Entry{});

  REQUIRE(buffer.Entries()[0].clusters != nullptr);
  CHECK(buffer.Entries()[0].clusters->Count(Lib::ClusterCondition::Intact) ==
        9);
  CHECK(buffer.Entries()[1].clusters == nullptr);
}

TEST_CASE("An EntryBuffer starts again after Clear", "[search][buffer]") {
  Lib::EntryBuffer buffer;
  for (std::size_t round = 0; round < 3; ++round) {
    for (std::size_t n = 0; n < 200; ++n) {
      Lib::Entry entry;
      const std::u16string name = NameOf(n + round);
      entry.name = name;
      buffer.Add(entry);
    }
    REQUIRE(buffer.Size() == 200);
    CHECK(buffer.Entries()[199].name == NameOf(199 + round));
    buffer.Clear();
    CHECK(buffer.Size() == 0);
  }
}
