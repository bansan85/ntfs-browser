#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using Bpb = NTFS_BROWSER_NAMESPACE::Data::NtfsBpb;

namespace {

// Prefix of the message logged when a field that must be zero is not.
constexpr const char* reserved_message =
    "Boot sector fields that must be zero are not: ";

// First message of the geometry log, which the check MUST precede.
constexpr const char* geometry_message = "Sector Size = ";

// Message of the end marker check, which runs before the reserved fields.
constexpr const char* marker_message = "Boot sector end marker is not 0xAA55";

// A BPB region that a real NTFS volume keeps at zero.
struct ZeroRegion {
  // Name the message gives for the region.
  std::string_view name;
  // Offset of the region in the boot sector.
  size_t offset;
  // Size of the region in bytes.
  size_t size;
};

// The regions the check enforces, at the offsets the NTFS layout gives.
constexpr std::array<ZeroRegion, 5> zero_regions{{
    {"reserved_sectors (0x0E)", 0x0E, 2},
    {"fat_count_and_root_entries (0x10)", 0x10, 3},
    {"small_sectors (0x13)", 0x13, 2},
    {"sectors_per_fat (0x16)", 0x16, 2},
    {"large_sectors (0x20)", 0x20, 4},
}};

static_assert(offsetof(Bpb, reserved_sectors) == 0x0E);
static_assert(offsetof(Bpb, zeros1) == 0x10);
static_assert(offsetof(Bpb, not_used1) == 0x13);
static_assert(offsetof(Bpb, zeros2) == 0x16);
static_assert(offsetof(Bpb, not_used2) == 0x20);
static_assert(offsetof(Bpb, not_used3) == 0x24);

// BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
std::vector<BYTE> ImageWithBpb(Edit edit) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  edit(*reinterpret_cast<Bpb*>(image.data()));
  return image;
}

// Image whose boot sector byte at the given offset holds 0xFF.
std::vector<BYTE> ImageWithByteSet(size_t offset) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  image.at(offset) = 0xFF;
  return image;
}

// What opening an image logged, and whether the volume came up.
struct Opened {
  bool ok{false};
  WORD sector_size{0};
  DWORD cluster_size{0};
  std::string log;
};

template <Cache::Strategy S>
Opened Open(std::vector<BYTE> image, VolumeOptions options = {}) {
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)), options);
  return {.ok = volume.IsVolumeOK(),
          .sector_size = volume.GetSectorSize(),
          .cluster_size = volume.GetClusterSize(),
          .log = NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog()};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("NtfsVolume checks the BPB reserved fields are zero",
                       "[ntfs-volume][boot-sector]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const VolumeOptions recover{.recover_errors = true};

  SECTION("a valid image is accepted and logs no reserved fields message") {
    const Opened opened =
        Open<S>(NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage());

    CHECK(opened.ok);
    CHECK_THAT(opened.log, !ContainsSubstring(reserved_message));
    CHECK_THAT(opened.log, ContainsSubstring(geometry_message));
  }

  SECTION("recover mode keeps a valid image silent") {
    const Opened opened =
        Open<S>(NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage(), recover);

    CHECK(opened.ok);
    CHECK_THAT(opened.log, !ContainsSubstring(reserved_message));
  }

  SECTION("strict mode rejects each region before decoding the geometry") {
    for (const ZeroRegion& region : zero_regions) {
      DYNAMIC_SECTION(region.name) {
        const Opened opened = Open<S>(ImageWithByteSet(region.offset));

        CHECK_FALSE(opened.ok);
        CHECK_THAT(opened.log, ContainsSubstring(reserved_message));
        CHECK_THAT(opened.log, ContainsSubstring(std::string(region.name)));
        CHECK_THAT(opened.log, !ContainsSubstring(geometry_message));
        CHECK(opened.sector_size == 0);
        CHECK(opened.cluster_size == 0);
      }
    }
  }

  SECTION("strict mode rejects each single byte of each region") {
    for (const ZeroRegion& region : zero_regions) {
      for (size_t byte = 0; byte < region.size; ++byte) {
        DYNAMIC_SECTION(region.name << " byte " << byte) {
          const Opened opened = Open<S>(ImageWithByteSet(region.offset + byte));

          CHECK_FALSE(opened.ok);
          CHECK_THAT(opened.log, ContainsSubstring(std::string(region.name)));
        }
      }
    }
  }

  SECTION("a one-bit value is enough to reject") {
    const Opened opened =
        Open<S>(ImageWithBpb([](Bpb& bpb) { bpb.reserved_sectors = 1; }));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(reserved_message));
  }

  SECTION("recover mode accepts each region and decodes the geometry") {
    for (const ZeroRegion& region : zero_regions) {
      DYNAMIC_SECTION(region.name) {
        const Opened opened = Open<S>(ImageWithByteSet(region.offset), recover);

        CHECK(opened.ok);
        CHECK(opened.sector_size ==
              NTFS_BROWSER_NAMESPACE::Tests::fake_bytes_per_sector);
        CHECK(opened.cluster_size ==
              NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
        CHECK_THAT(opened.log, ContainsSubstring(reserved_message));
        CHECK_THAT(opened.log, ContainsSubstring(std::string(region.name)));
        CHECK_THAT(opened.log, ContainsSubstring(geometry_message));
        // The check is reported first, before any geometry line.
        CHECK(opened.log.find(reserved_message) <
              opened.log.find(geometry_message));
      }
    }
  }

  SECTION("one message names every offending region") {
    const Opened opened = Open<S>(ImageWithBpb([](Bpb& bpb) {
      bpb.reserved_sectors = 1;
      bpb.not_used1 = 1;
      bpb.not_used2 = 1;
    }));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring("reserved_sectors (0x0E)"));
    CHECK_THAT(opened.log, ContainsSubstring("small_sectors (0x13)"));
    CHECK_THAT(opened.log, ContainsSubstring("large_sectors (0x20)"));
    CHECK_THAT(opened.log, !ContainsSubstring("sectors_per_fat"));
    CHECK_THAT(opened.log, !ContainsSubstring("fat_count_and_root_entries"));
    // One message, not one per region.
    const size_t first = opened.log.find(reserved_message);
    REQUIRE(first != std::string::npos);
    CHECK(opened.log.find(reserved_message, first + 1) == std::string::npos);
  }

  SECTION("recover mode logs the message once for several regions") {
    const Opened opened = Open<S>(ImageWithBpb([](Bpb& bpb) {
                                    bpb.zeros2 = 0xFFFF;
                                    bpb.not_used2 = 0xFFFFFFFF;
                                  }),
                                  recover);

    CHECK(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring("sectors_per_fat (0x16)"));
    CHECK_THAT(opened.log, ContainsSubstring("large_sectors (0x20)"));
  }

  SECTION("fields real volumes fill in are accepted silently") {
    const Opened opened = Open<S>(ImageWithBpb([](Bpb& bpb) {
      bpb.media_descriptor = 0xF8;
      bpb.sectors_per_track = 63;
      bpb.number_of_heads = 255;
      bpb.hidden_sectors = 2048;
      bpb.not_used3 = 0x80008000;
    }));

    CHECK(opened.ok);
    CHECK_THAT(opened.log, !ContainsSubstring(reserved_message));
  }

  SECTION("each unchecked field is accepted on its own") {
    constexpr std::array<size_t, 5> unchecked{
        offsetof(Bpb, media_descriptor), offsetof(Bpb, sectors_per_track),
        offsetof(Bpb, number_of_heads),  offsetof(Bpb, hidden_sectors),
        offsetof(Bpb, not_used3),
    };
    for (const size_t offset : unchecked) {
      DYNAMIC_SECTION("offset " << offset) {
        const Opened opened = Open<S>(ImageWithByteSet(offset));

        CHECK(opened.ok);
        CHECK_THAT(opened.log, !ContainsSubstring(reserved_message));
      }
    }
  }

  SECTION("a missing marker is reported before the reserved fields") {
    const Opened opened = Open<S>(ImageWithBpb([](Bpb& bpb) {
      bpb.x_aa = 0;
      bpb.reserved_sectors = 1;
    }));

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(marker_message));
    CHECK_THAT(opened.log, !ContainsSubstring(reserved_message));
  }

  SECTION("recover mode reports the marker, then the reserved fields") {
    const Opened opened = Open<S>(ImageWithBpb([](Bpb& bpb) {
                                    bpb.x_aa = 0;
                                    bpb.reserved_sectors = 1;
                                  }),
                                  recover);

    CHECK(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(marker_message));
    CHECK_THAT(opened.log, ContainsSubstring(reserved_message));
    CHECK(opened.log.find(marker_message) < opened.log.find(reserved_message));
    CHECK(opened.log.find(reserved_message) <
          opened.log.find(geometry_message));
  }

  SECTION("a bad signature is still reported first") {
    const Opened opened = Open<S>(ImageWithBpb([](Bpb& bpb) {
                                    bpb.signature[0] = 'X';
                                    bpb.reserved_sectors = 1;
                                  }),
                                  recover);

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring("Volume file system is not NTFS"));
    CHECK_THAT(opened.log, !ContainsSubstring(reserved_message));
  }

  SECTION("a bad sector size is still rejected in recover mode") {
    const Opened opened = Open<S>(ImageWithBpb([](Bpb& bpb) {
                                    bpb.bytes_per_sector = 3;
                                    bpb.reserved_sectors = 1;
                                  }),
                                  recover);

    CHECK_FALSE(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(reserved_message));
    CHECK_THAT(opened.log, ContainsSubstring("Sector Size must be"));
  }
}
