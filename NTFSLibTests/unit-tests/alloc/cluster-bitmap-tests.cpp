#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "alloc/cluster-bitmap.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
using Alloc::ClusterBitmap;
using Alloc::ClusterState;
using Alloc::RangeQuery;

namespace {

// Bytes of the small windows these tests use: 128 clusters each.
constexpr size_t small_window = 16;

// Clusters one small window covers.
constexpr ULONGLONG small_window_clusters = small_window * 8;

// Clusters one fake bitmap cluster of 1 KiB tracks.
constexpr ULONGLONG bits_per_bitmap_cluster = 1024 * 8;

// A reader over a byte vector that records every call it gets.
struct RecordingReader {
  std::shared_ptr<std::vector<BYTE>> bytes;
  std::shared_ptr<std::vector<ULONGLONG>> offsets =
      std::make_shared<std::vector<ULONGLONG>>();
  // Bytes to return at most per call, or all when empty.
  std::optional<size_t> cap;
  // Offsets of windows that fail outright.
  std::vector<ULONGLONG> failing;
  bool throws{false};

  ClusterBitmap::Reader Make() const {
    return [*this](ULONGLONG offset,
                   std::span<BYTE> out) -> std::optional<ULONGLONG> {
      offsets->push_back(offset);
      if (throws) {
        throw std::runtime_error("read failed");
      }
      if (std::ranges::find(failing, offset) != failing.end()) {
        return std::nullopt;
      }
      if (offset >= bytes->size()) {
        return std::nullopt;
      }
      size_t count = std::min(out.size(), bytes->size() - offset);
      if (cap) {
        count = std::min(count, *cap);
      }
      std::memcpy(out.data(), bytes->data() + offset, count);
      return count;
    };
  }
};

RecordingReader ReaderOver(std::vector<BYTE> bytes) {
  RecordingReader reader;
  reader.bytes = std::make_shared<std::vector<BYTE>>(std::move(bytes));
  return reader;
}

// Sets bit lcn of bitmap.
void SetBit(std::vector<BYTE>& bitmap, ULONGLONG lcn) {
  bitmap.at(lcn / 8) |= static_cast<BYTE>(1U << (lcn % 8));
}

}  // namespace

TEST_CASE("a cluster bitmap reads bits least significant first",
          "[alloc][cluster-bitmap]") {
  const RecordingReader reader = ReaderOver({0x01, 0x80, 0x00});
  ClusterBitmap bitmap(reader.Make(), 3, 0);

  CHECK(bitmap.State(0) == ClusterState::Allocated);
  CHECK(bitmap.State(1) == ClusterState::Free);
  CHECK(bitmap.State(7) == ClusterState::Free);
  CHECK(bitmap.State(15) == ClusterState::Allocated);
  CHECK(bitmap.State(16) == ClusterState::Free);
  CHECK(bitmap.State(23) == ClusterState::Free);
}

TEST_CASE("a cluster past the end of the volume is out of range",
          "[alloc][cluster-bitmap]") {
  const RecordingReader reader = ReaderOver({0x00, 0x00});

  SECTION("the bitmap's own bit count bounds an unknown volume") {
    ClusterBitmap bitmap(reader.Make(), 2, 0);

    CHECK(bitmap.State(15) == ClusterState::Free);
    CHECK(bitmap.State(16) == ClusterState::OutOfRange);
    CHECK(bitmap.State(128) == ClusterState::OutOfRange);
    CHECK(bitmap.State(std::numeric_limits<ULONGLONG>::max()) ==
          ClusterState::OutOfRange);
  }

  SECTION("an empty bitmap has no cluster") {
    ClusterBitmap bitmap(reader.Make(), 0, 0);

    CHECK(bitmap.State(0) == ClusterState::OutOfRange);
    CHECK_FALSE(bitmap.Summary(1024));
    CHECK(reader.offsets->empty());
  }
}

TEST_CASE("padding bits past the last cluster are ignored",
          "[alloc][cluster-bitmap]") {
  // Twenty clusters: the last byte's bits 4 to 7 are padding, all set.
  const RecordingReader reader = ReaderOver({0x00, 0x00, 0xF0});
  ClusterBitmap bitmap(reader.Make(), 3, 20);

  CHECK(bitmap.State(19) == ClusterState::Free);
  CHECK(bitmap.State(20) == ClusterState::OutOfRange);

  const auto summary = bitmap.Summary(1024);
  REQUIRE(summary);
  CHECK(summary->total_clusters == 20);
  CHECK(summary->free_clusters == 20);
  CHECK(summary->free_bytes == 20 * 1024);
  CHECK(summary->unknown_clusters == 0);
}

TEST_CASE("a range counts what it judged and clamps to the volume",
          "[alloc][cluster-bitmap]") {
  std::vector<BYTE> bytes(4, 0x00);
  SECTION("a range inside the volume") {
    SetBit(bytes, 17);
    ClusterBitmap bitmap(ReaderOver(bytes).Make(), 4, 32);
    RangeQuery query{.lcn = 16, .clusters = 8};
    bitmap.StatesInOrder({&query, 1});

    CHECK(query.state == ClusterState::Allocated);
    CHECK(query.allocated_clusters == 1);
    CHECK(query.free_clusters == 7);
  }

  SECTION("a range past the end still judges its inside part") {
    SetBit(bytes, 17);
    ClusterBitmap bitmap(ReaderOver(bytes).Make(), 4, 20);
    RangeQuery query{.lcn = 16, .clusters = 10};
    bitmap.StatesInOrder({&query, 1});

    CHECK(query.state == ClusterState::Allocated);
    CHECK(query.allocated_clusters == 1);
    CHECK(query.free_clusters == 3);
  }

  SECTION("a free range that crosses the end is out of range") {
    ClusterBitmap bitmap(ReaderOver(bytes).Make(), 4, 20);
    RangeQuery query{.lcn = 16, .clusters = 10};
    bitmap.StatesInOrder({&query, 1});

    CHECK(query.state == ClusterState::OutOfRange);
    CHECK(query.allocated_clusters == 0);
    CHECK(query.free_clusters == 4);
  }

  SECTION("an empty range is free") {
    ClusterBitmap bitmap(ReaderOver(bytes).Make(), 4, 32);

    CHECK(bitmap.RangeState(5, 0) == ClusterState::Free);
  }

  SECTION("a range whose end overflows 64 bits does not throw") {
    ClusterBitmap bitmap(ReaderOver(bytes).Make(), 4, 32);

    CHECK(bitmap.RangeState(5, std::numeric_limits<ULONGLONG>::max()) ==
          ClusterState::OutOfRange);
    CHECK(bitmap.RangeState(std::numeric_limits<ULONGLONG>::max() - 1, 10) ==
          ClusterState::OutOfRange);
  }
}

TEST_CASE("an error never hides a positive finding",
          "[alloc][cluster-bitmap]") {
  // The bitmap holds 16 bits, and the volume has 24 clusters.
  std::vector<BYTE> bytes(2, 0x00);

  SECTION("past the bitmap, clusters are unknown") {
    ClusterBitmap bitmap(ReaderOver(bytes).Make(), 2, 24);

    CHECK(bitmap.RangeState(14, 4) == ClusterState::Unknown);
    CHECK(bitmap.State(20) == ClusterState::Unknown);
    CHECK(bitmap.State(24) == ClusterState::OutOfRange);
    CHECK(bitmap.GetStatus().shorter_than_volume);
  }

  SECTION("out of range wins over unknown") {
    ClusterBitmap bitmap(ReaderOver(bytes).Make(), 2, 24);

    CHECK(bitmap.RangeState(14, 20) == ClusterState::OutOfRange);
  }

  SECTION("allocated wins over both") {
    SetBit(bytes, 3);
    ClusterBitmap bitmap(ReaderOver(bytes).Make(), 2, 24);

    CHECK(bitmap.RangeState(0, 40) == ClusterState::Allocated);
  }
}

TEST_CASE("free space of a bitmap shorter than the volume",
          "[alloc][cluster-bitmap]") {
  const RecordingReader reader = ReaderOver({0x0F, 0x00});
  ClusterBitmap bitmap(reader.Make(), 2, 24);

  const auto summary = bitmap.Summary(1024);
  REQUIRE(summary);
  CHECK(summary->total_clusters == 24);
  CHECK(summary->free_clusters == 12);
  CHECK(summary->unknown_clusters == 8);
}

TEST_CASE("free bytes are the free clusters times the cluster size",
          "[alloc][cluster-bitmap]") {
  const RecordingReader reader = ReaderOver({0x00});
  ClusterBitmap bitmap(reader.Make(), 1, 8);

  const auto summary = bitmap.Summary(std::numeric_limits<DWORD>::max());
  REQUIRE(summary);
  CHECK(summary->free_clusters == 8);
  CHECK(summary->free_bytes == 8ULL * std::numeric_limits<DWORD>::max());
}

TEST_CASE("windows cut the bitmap at their boundary",
          "[alloc][cluster-bitmap]") {
  std::vector<BYTE> bytes(small_window * 4, 0x00);
  SetBit(bytes, small_window_clusters - 1);
  SetBit(bytes, small_window_clusters);
  const RecordingReader reader = ReaderOver(bytes);
  ClusterBitmap bitmap(reader.Make(), bytes.size(), 0, small_window);

  CHECK(bitmap.State(small_window_clusters - 2) == ClusterState::Free);
  CHECK(bitmap.State(small_window_clusters - 1) == ClusterState::Allocated);
  CHECK(bitmap.State(small_window_clusters) == ClusterState::Allocated);
  CHECK(bitmap.State(small_window_clusters + 1) == ClusterState::Free);

  RangeQuery across{.lcn = small_window_clusters - 4, .clusters = 8};
  bitmap.StatesInOrder({&across, 1});
  CHECK(across.allocated_clusters == 2);
  CHECK(across.free_clusters == 6);
}

TEST_CASE("a three window bitmap is queried cluster by cluster in one window",
          "[alloc][cluster-bitmap]") {
  std::vector<BYTE> bytes(small_window * 3, 0x55);
  const RecordingReader reader = ReaderOver(bytes);
  ClusterBitmap bitmap(reader.Make(), bytes.size(), 0, small_window);

  for (ULONGLONG lcn = 0; lcn < small_window_clusters * 3; lcn++) {
    CHECK(bitmap.State(lcn) ==
          (lcn % 2 == 0 ? ClusterState::Allocated : ClusterState::Free));
    CHECK(bitmap.BytesHeld() <= small_window);
  }
  CHECK(*reader.offsets == std::vector<ULONGLONG>{0, 16, 32});
}

TEST_CASE("a batch reads each window once, in LCN order",
          "[alloc][cluster-bitmap]") {
  std::vector<BYTE> bytes(small_window * 3, 0x00);
  SetBit(bytes, 5);
  SetBit(bytes, 2 * small_window_clusters + 7);
  const RecordingReader reader = ReaderOver(bytes);
  ClusterBitmap bitmap(reader.Make(), bytes.size(), 0, small_window);

  std::vector<RangeQuery> queries{
      {.lcn = 2 * small_window_clusters + 7, .clusters = 1},
      {.lcn = 1, .clusters = 8},
      {.lcn = small_window_clusters + 3, .clusters = 2},
      {.lcn = 2 * small_window_clusters, .clusters = 4},
      {.lcn = 4, .clusters = 2},
  };
  bitmap.StatesInOrder(queries);

  CHECK(queries[0].state == ClusterState::Allocated);
  CHECK(queries[1].state == ClusterState::Allocated);
  CHECK(queries[2].state == ClusterState::Free);
  CHECK(queries[3].state == ClusterState::Free);
  CHECK(queries[4].state == ClusterState::Allocated);
  CHECK(queries[0].lcn == 2 * small_window_clusters + 7);
  CHECK(*reader.offsets == std::vector<ULONGLONG>{0, 16, 32});
}

TEST_CASE("a window read that stops short keeps what it returned",
          "[alloc][cluster-bitmap]") {
  std::vector<BYTE> bytes(small_window * 2, 0x00);
  SetBit(bytes, 3);
  RecordingReader reader = ReaderOver(bytes);
  reader.cap = 8;
  ClusterBitmap bitmap(reader.Make(), bytes.size(), 0, small_window);

  SECTION("the bytes returned answer while the window is held") {
    CHECK(bitmap.State(3) == ClusterState::Allocated);
    CHECK(bitmap.State(4) == ClusterState::Free);
    CHECK(bitmap.State(63) == ClusterState::Free);
    CHECK(bitmap.State(64) == ClusterState::Unknown);
    CHECK(bitmap.State(127) == ClusterState::Unknown);
    CHECK(bitmap.GetStatus().failed_windows == 1);
  }

  SECTION("the window is not read again once it is dropped") {
    CHECK(bitmap.State(3) == ClusterState::Allocated);
    CHECK(bitmap.State(small_window_clusters + 1) == ClusterState::Free);
    CHECK(bitmap.State(3) == ClusterState::Unknown);
    CHECK(std::ranges::count(*reader.offsets, ULONGLONG{0}) == 1);
    CHECK(bitmap.GetStatus().failed_windows == 2);
  }
}

TEST_CASE("a window that fails leaves the others answering",
          "[alloc][cluster-bitmap]") {
  std::vector<BYTE> bytes(small_window * 3, 0x00);
  SetBit(bytes, 2 * small_window_clusters + 1);
  RecordingReader reader = ReaderOver(bytes);
  reader.failing = {small_window};
  ClusterBitmap bitmap(reader.Make(), bytes.size(), 0, small_window);

  CHECK(bitmap.State(1) == ClusterState::Free);
  CHECK(bitmap.State(small_window_clusters + 1) == ClusterState::Unknown);
  CHECK(bitmap.State(small_window_clusters + 2) == ClusterState::Unknown);
  CHECK(bitmap.State(2 * small_window_clusters + 1) == ClusterState::Allocated);
  CHECK(bitmap.State(small_window_clusters + 1) == ClusterState::Unknown);
  CHECK(std::ranges::count(*reader.offsets, small_window) == 1);
  CHECK(bitmap.GetStatus().failed_windows == 1);

  SECTION("a positive finding survives the failed window in a range") {
    CHECK(bitmap.RangeState(0, 3 * small_window_clusters) ==
          ClusterState::Allocated);
  }
}

TEST_CASE("a reader that throws is contained", "[alloc][cluster-bitmap]") {
  RecordingReader reader = ReaderOver({0x00, 0x00});
  reader.throws = true;
  ClusterBitmap bitmap(reader.Make(), 2, 16);

  CHECK(bitmap.State(0) == ClusterState::Unknown);
  CHECK(bitmap.State(0) == ClusterState::Unknown);
  CHECK(reader.offsets->size() == 1);
  CHECK_FALSE(bitmap.Summary(1024));
}

TEST_CASE("a forged volume size cannot make a query long",
          "[alloc][cluster-bitmap]") {
  // A bitmap of a petabyte for a volume of 2^60 clusters, none of it readable.
  RecordingReader reader = ReaderOver({});
  reader.bytes = std::make_shared<std::vector<BYTE>>();
  ClusterBitmap bitmap(reader.Make(), ULONGLONG{1} << 50, ULONGLONG{1} << 60,
                       small_window);

  CHECK(bitmap.RangeState(0, ULONGLONG{1} << 59) == ClusterState::Unknown);
  CHECK_FALSE(bitmap.Summary(1024));
  CHECK(bitmap.State(ULONGLONG{1} << 58) == ClusterState::Unknown);
  // The reads stop once too many windows have failed.
  CHECK(reader.offsets->size() <= 4096);
}

TEST_CASE("a range past the end of a short bitmap is not walked",
          "[alloc][cluster-bitmap]") {
  // The volume claims 2^60 clusters, the bitmap holds 16 bits.
  const RecordingReader reader = ReaderOver({0x00, 0x00});
  ClusterBitmap bitmap(reader.Make(), 2, ULONGLONG{1} << 60);

  CHECK(bitmap.RangeState(0, ULONGLONG{1} << 59) == ClusterState::Unknown);
  CHECK(reader.offsets->size() == 1);

  const auto summary = bitmap.Summary(1024);
  REQUIRE(summary);
  CHECK(summary->free_clusters == 16);
  CHECK(summary->unknown_clusters == (ULONGLONG{1} << 60) - 16);
}

TEST_CASE("a three cluster bitmap answers past its first cluster",
          "[alloc][cluster-bitmap]") {
  // Ported from the old AttrBitmap lookup: the first bitmap cluster is all
  // ones, the second all zeros, the third has only bit 0 set.
  std::vector<BYTE> bytes(3 * 1024, 0x00);
  std::fill_n(bytes.begin(), 1024, BYTE{0xFF});
  bytes.at(2048) = 0x01;
  ClusterBitmap bitmap(ReaderOver(bytes).Make(), bytes.size(),
                       3 * bits_per_bitmap_cluster, 1024);

  CHECK(bitmap.State(bits_per_bitmap_cluster) == ClusterState::Free);
  CHECK(bitmap.State(bits_per_bitmap_cluster + 7) == ClusterState::Free);
  CHECK(bitmap.State(2 * bits_per_bitmap_cluster) == ClusterState::Allocated);
  CHECK(bitmap.State(2 * bits_per_bitmap_cluster + 1) == ClusterState::Free);
  CHECK(bitmap.State(0) == ClusterState::Allocated);
  CHECK(bitmap.State(bits_per_bitmap_cluster - 1) == ClusterState::Allocated);
  CHECK(bitmap.State(bits_per_bitmap_cluster + 1) == ClusterState::Free);
}

TEST_CASE("a two byte bitmap answers from its own bytes and past its end",
          "[alloc][cluster-bitmap]") {
  // Ported from the old resident AttrBitmap lookup. Past its end a cluster is
  // now out of range, no longer reported free.
  ClusterBitmap bitmap(ReaderOver({0x01, 0x00}).Make(), 2, 0);

  CHECK(bitmap.State(0) == ClusterState::Allocated);
  CHECK(bitmap.State(1) == ClusterState::Free);
  CHECK(bitmap.State(8) == ClusterState::Free);
  CHECK(bitmap.State(128) == ClusterState::OutOfRange);
}

TEST_CASE("a bitmap whose read comes back short reports no free cluster",
          "[alloc][cluster-bitmap]") {
  // Ported from the old non-resident lookup whose cluster read came back short.
  RecordingReader reader = ReaderOver({0x00, 0x00});
  reader.cap = 0;
  ClusterBitmap bitmap(reader.Make(), 2, 0);

  CHECK(bitmap.State(0) == ClusterState::Unknown);
  CHECK(bitmap.State(0) == ClusterState::Unknown);
  CHECK(reader.offsets->size() == 1);
}
