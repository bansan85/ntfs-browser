#include <cstddef>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "attr/index-entry-walker.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "index-entry.h"
#include "win-types.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Data = NTFS_BROWSER_NAMESPACE::Data;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;

namespace {

// Size of an index entry with no stream: just its fixed part.
constexpr WORD bare_entry_size = offsetof(Data::IndexEntry, stream);

// One zeroed entry of `size` bytes, with the given flags, appended to bytes.
void AppendEntry(std::vector<BYTE>& bytes, WORD size,
                 Data::IndexEntryFlag flags) {
  std::vector<BYTE> entry(size);
  std::memcpy(&entry.at(offsetof(Data::IndexEntry, size)), &size, sizeof(size));
  entry.at(offsetof(Data::IndexEntry, flags)) = static_cast<BYTE>(flags);
  bytes.insert(bytes.end(), entry.begin(), entry.end());
}

// A walk over a buffer, with the lists it fills.
struct WalkCase {
  std::vector<IndexEntryView> entries;
  std::vector<std::vector<BYTE>> realigned;

  Attr::IndexEntryWalker::Outcome
      Run(bool recover, const std::vector<BYTE>& bytes, DWORD declared_total) {
    Attr::IndexEntryWalker walker(Attr::IndexEntryWalker::block_messages,
                                  recover, entries, realigned);
    return walker.Walk(bytes, declared_total);
  }
};

}  // namespace

TEST_CASE("The walker gives every entry and where the end entry stops",
          "[attr][index-walker]") {
  std::vector<BYTE> bytes;
  AppendEntry(bytes, bare_entry_size, Data::IndexEntryFlag::None);
  AppendEntry(bytes, bare_entry_size, Data::IndexEntryFlag::Last);

  WalkCase walk;
  const auto outcome = walk.Run(false, bytes, 2U * bare_entry_size);

  CHECK(outcome.accepted);
  CHECK(outcome.clean_end == 2U * bare_entry_size);
  CHECK(walk.entries.size() == 2);
}

TEST_CASE("The walker empties the entries when their total is too big",
          "[attr][index-walker]") {
  std::vector<BYTE> bytes;
  AppendEntry(bytes, bare_entry_size, Data::IndexEntryFlag::None);
  AppendEntry(bytes, bare_entry_size, Data::IndexEntryFlag::Last);

  WalkCase strict;
  const auto rejected = strict.Run(false, bytes, bare_entry_size);
  CHECK_FALSE(rejected.accepted);
  CHECK(strict.entries.empty());

  WalkCase recovering;
  const auto kept = recovering.Run(true, bytes, bare_entry_size);
  CHECK(kept.accepted);
  CHECK_FALSE(kept.clean_end.has_value());
  CHECK(recovering.entries.size() == 1);
}

TEST_CASE("The walker keeps what it has when the bytes end before an entry",
          "[attr][index-walker]") {
  std::vector<BYTE> bytes;
  AppendEntry(bytes, bare_entry_size, Data::IndexEntryFlag::None);

  WalkCase strict;
  CHECK_FALSE(strict.Run(false, bytes, 2U * bare_entry_size).accepted);
  CHECK(strict.entries.empty());

  WalkCase recovering;
  const auto kept = recovering.Run(true, bytes, 2U * bare_entry_size);
  CHECK(kept.accepted);
  CHECK_FALSE(kept.clean_end.has_value());
  CHECK(recovering.entries.size() == 1);
}

TEST_CASE("The walker gives no end after a defect that recovery let pass",
          "[attr][index-walker]") {
  // A sub-node entry too small to hold its VCN field.
  std::vector<BYTE> bytes;
  AppendEntry(bytes, bare_entry_size, Data::IndexEntryFlag::SubNode);
  AppendEntry(bytes, bare_entry_size, Data::IndexEntryFlag::Last);

  WalkCase recovering;
  const auto outcome = recovering.Run(true, bytes, 2U * bare_entry_size);
  CHECK(outcome.accepted);
  CHECK_FALSE(outcome.clean_end.has_value());
  CHECK(recovering.entries.size() == 2);

  WalkCase strict;
  CHECK_FALSE(strict.Run(false, bytes, 2U * bare_entry_size).accepted);
  CHECK(strict.entries.empty());
}
