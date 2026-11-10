#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "alloc/bitmap-windows.h"
#include "win-types.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
using Alloc::BitmapWindows;

namespace {

// Bytes of one window in these tests.
constexpr size_t window_bytes = 8;

// Bytes of the whole bitmap: three windows, the last one short.
constexpr ULONGLONG bitmap_bytes = 20;

// A reader that fills each byte with its offset, and counts its calls. A
// window at or past failing_from reads nothing.
struct CountingReader {
  size_t* calls;
  ULONGLONG failing_from;

  std::optional<ULONGLONG> operator()(ULONGLONG offset,
                                      std::span<BYTE> out) const {
    (*calls)++;
    if (offset >= failing_from) {
      return std::nullopt;
    }
    for (size_t i = 0; i < out.size(); i++) {
      out[i] = static_cast<BYTE>(offset + i);
    }
    return out.size();
  }
};

BitmapWindows Make(size_t& calls, ULONGLONG failing_from = bitmap_bytes) {
  return {CountingReader{&calls, failing_from}, bitmap_bytes, window_bytes};
}

}  // namespace

TEST_CASE("a window holds the bytes at its index", "[alloc][bitmap-windows]") {
  size_t calls = 0;
  BitmapWindows windows = Make(calls);

  const BitmapWindows::Window* last = windows.Load(2);

  REQUIRE(last != nullptr);
  CHECK(last->bytes.size() == bitmap_bytes - 2 * window_bytes);
  CHECK(last->valid == last->bytes.size());
  CHECK(last->bytes.front() == 2 * window_bytes);
}

TEST_CASE("only the window held is read again without retention",
          "[alloc][bitmap-windows]") {
  size_t calls = 0;
  BitmapWindows windows = Make(calls);

  CHECK(windows.Load(0) != nullptr);
  CHECK(windows.Load(0) != nullptr);
  CHECK(calls == 1);
  CHECK(windows.Load(1) != nullptr);
  CHECK(windows.Load(0) != nullptr);
  CHECK(calls == 3);
}

TEST_CASE("retained windows are not read again", "[alloc][bitmap-windows]") {
  size_t calls = 0;
  BitmapWindows windows = Make(calls);
  windows.SetRetention(2 * window_bytes);

  CHECK(windows.Load(0) != nullptr);
  CHECK(windows.Load(1) != nullptr);
  CHECK(windows.Load(2) != nullptr);
  CHECK(windows.Load(0) != nullptr);
  CHECK(windows.Load(1) != nullptr);

  CHECK(calls == 3);
  CHECK(windows.BytesHeld() >= 2 * window_bytes);
  windows.SetRetention(0);
  CHECK(windows.BytesHeld() <= window_bytes);
}

TEST_CASE("a failed window is remembered and not read again",
          "[alloc][bitmap-windows]") {
  size_t calls = 0;
  BitmapWindows windows = Make(calls, window_bytes);

  const BitmapWindows::Window* first = windows.Load(1);
  REQUIRE(first != nullptr);
  CHECK(first->valid == 0);
  CHECK(windows.FailedCount() == 1);
  CHECK(windows.Load(0) != nullptr);
  CHECK(windows.Load(1) == nullptr);
  CHECK(calls == 2);
  CHECK_FALSE(windows.GaveUp());
}
