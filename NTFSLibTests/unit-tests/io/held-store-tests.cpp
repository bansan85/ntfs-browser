#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "io/directory-read-ahead.h"
#include "io/read-ahead-held.h"
#include "win-types.h"

namespace Io = NTFS_BROWSER_NAMESPACE::Io;

namespace {

// Bytes of the record and of the blocks the tests hold.
constexpr std::size_t record_bytes = 64;
constexpr std::size_t block_bytes = 128;

}  // namespace

TEST_CASE("a held store knows nothing of a directory it was not given",
          "[io][held-store]") {
  Io::DirectoryReadAheadStats stats;
  Io::HeldStore store(stats);

  CHECK_FALSE(store.Contains(7));
  CHECK_FALSE(store.ContainsBlock(7, 0));
  CHECK_FALSE(store.HasRecord(7));
  CHECK(store.ServeRecord(7).empty());
  CHECK(store.ServeBlock(7, 0).empty());
  CHECK(store.PeekBlock(7, 0).empty());
  CHECK(store.BytesOf(7) == 0);
  CHECK(store.Bytes() == 0);
  store.Release(7);
  CHECK(stats.records_unused == 0);
}

TEST_CASE("a held store serves a record and a block once each",
          "[io][held-store]") {
  Io::DirectoryReadAheadStats stats;
  Io::HeldStore store(stats);
  store.HoldRecord(7, std::vector<BYTE>(record_bytes, 1));
  store.HoldBlock(7, 3, std::vector<BYTE>(block_bytes, 2));

  CHECK(store.Contains(7));
  CHECK(store.HasRecord(7));
  CHECK(store.ContainsBlock(7, 3));
  CHECK_FALSE(store.ContainsBlock(7, 4));
  CHECK(store.Bytes() == record_bytes + block_bytes);
  CHECK(store.BytesOf(7) == record_bytes + block_bytes);
  CHECK(store.RecordOf(7).size() == record_bytes);

  CHECK(store.ServeRecord(7).size() == record_bytes);
  CHECK(store.ServeRecord(7).size() == record_bytes);
  CHECK(stats.records_served == 1);
  CHECK(store.PeekBlock(7, 3).size() == block_bytes);
  CHECK(stats.blocks_served == 0);
  CHECK(store.ServeBlock(7, 3).size() == block_bytes);
  CHECK(store.ServeBlock(7, 3).size() == block_bytes);
  CHECK(stats.blocks_served == 1);
  CHECK(store.ServeBlock(7, 4).empty());
  CHECK(store.PeekBlock(7, 4).empty());
}

TEST_CASE("a held store counts what a release frees unused",
          "[io][held-store]") {
  Io::DirectoryReadAheadStats stats;
  Io::HeldStore store(stats);
  store.HoldRecord(7, std::vector<BYTE>(record_bytes, 1));
  store.HoldBlock(7, 3, std::vector<BYTE>(block_bytes, 2));
  store.HoldBlock(7, 4, std::vector<BYTE>(block_bytes, 3));
  static_cast<void>(store.ServeBlock(7, 3));

  store.Release(7);

  CHECK_FALSE(store.Contains(7));
  CHECK(store.Bytes() == 0);
  CHECK(stats.records_unused == 1);
  CHECK(stats.blocks_unused == 1);
  CHECK(stats.peak_held_bytes == record_bytes + 2 * block_bytes);
}
