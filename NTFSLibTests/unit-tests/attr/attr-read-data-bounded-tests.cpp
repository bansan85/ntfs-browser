#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "attr/non-resident.h"
#include "fake-ntfs-image.h"
#include "nonresident-index-support.h"
#include "optional-access.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

template <Cache::Strategy S>
class AttrNonResident;

}  // namespace Attr
}  // namespace NTFS_BROWSER_NAMESPACE

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// Fills the read buffer so bytes the read must not touch stay recognizable.
constexpr BYTE sentinel_byte = 0xCC;

// Bytes past the requested read that must keep the sentinel value.
constexpr size_t tail_slack = 8;

template <Cache::Strategy S>
const Attr::AttrNonResident<S>*
    ForgedAllocation(const NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>& record) {
  const auto& attrs = record.GetAttr(Attr::Type::IndexAllocation);
  REQUIRE(attrs.size() == 1);
  return dynamic_cast<const Attr::AttrNonResident<S>*>(attrs.front().get());
}

// Reads length bytes at offset and checks them against the same bytes of
// reference. Bytes past the read stay untouched.
template <Cache::Strategy S>
void CheckReadMatchesReference(const Attr::AttrNonResident<S>& attr,
                               const std::vector<BYTE>& reference,
                               ULONGLONG offset, size_t length) {
  REQUIRE(offset + length <= reference.size());
  std::vector<BYTE> out(length + tail_slack, sentinel_byte);

  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(attr.ReadExtentData(
            offset, std::span<BYTE>(out).first(length))) == length);
  CHECK(std::memcmp(out.data(), reference.data() + static_cast<size_t>(offset),
                    length) == 0);
  CHECK(std::all_of(out.begin() + static_cast<std::ptrdiff_t>(length),
                    out.end(), [](BYTE b) { return b == sentinel_byte; }));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("ReadExtentData bounds reads by the extent",
                       "[read-bounded][characterization]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  auto image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithForgedIndexBlock();
  auto parsed =
      OpenRecord<S>(image, NTFS_BROWSER_NAMESPACE::Tests::index_alloc_dir_idx);
  REQUIRE(parsed.record->ParseAttrs());
  const auto* alloc = ForgedAllocation<S>(*parsed.record);
  REQUIRE(alloc != nullptr);

  const ULONGLONG extent_bytes = alloc->GetAllocatedSize();
  REQUIRE(extent_bytes % NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size == 0);
  REQUIRE(extent_bytes >= 5 * NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);

  // One aligned read of the whole extent is the reference for every check.
  std::vector<BYTE> reference(static_cast<size_t>(extent_bytes), sentinel_byte);
  REQUIRE(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(alloc->ReadExtentData(
              0, std::span<BYTE>(reference))) == extent_bytes);
  REQUIRE(std::any_of(reference.begin(), reference.end(),
                      [](BYTE b) { return b != 0; }));

  SECTION("an empty buffer reads zero bytes") {
    CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(
              alloc->ReadExtentData(0, std::span<BYTE>{})) == 0);
    CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(
              alloc->ReadExtentData(extent_bytes + 1, std::span<BYTE>{})) == 0);
  }

  SECTION("an offset past the extent end fails") {
    std::vector<BYTE> out(16, sentinel_byte);
    CHECK_FALSE(alloc->ReadExtentData(extent_bytes + 1, out).has_value());
  }

  SECTION("an offset at the extent end reads zero bytes") {
    std::vector<BYTE> out(16, sentinel_byte);
    CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(
              alloc->ReadExtentData(extent_bytes, out)) == 0);
  }

  SECTION("a read straddling the extent end is clamped to the extent") {
    constexpr size_t tail = 100;
    const ULONGLONG offset = extent_bytes - tail;
    std::vector<BYTE> out(300, sentinel_byte);

    CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(
              alloc->ReadExtentData(offset, out)) == tail);
    CHECK(std::memcmp(out.data(),
                      reference.data() + static_cast<size_t>(offset),
                      tail) == 0);
    CHECK(std::all_of(out.begin() + tail, out.end(),
                      [](BYTE b) { return b == sentinel_byte; }));
  }

  SECTION("an unaligned read inside one cluster") {
    CheckReadMatchesReference<S>(*alloc, reference, 100, 200);
  }

  SECTION("an unaligned read over a partial, whole and partial cluster") {
    CheckReadMatchesReference<S>(*alloc, reference, 700, 4000);
  }

  SECTION("an aligned read of whole clusters") {
    CheckReadMatchesReference<S>(*alloc, reference, 2048, 2048);
  }
}
