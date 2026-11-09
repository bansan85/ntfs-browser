#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "cache/strategy.h"
#include "data/ntfs-bpb.h"
#include "ntfs-volume.h"
#include "volume-options.h"
#include "win-types.h"

namespace {

// Builds the image a boot sector scenario opens. It runs only for the
// scenario that is executed, so a long table stays cheap.
using BootImageMaker = std::function<std::vector<BYTE>()>;

// BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
std::vector<BYTE> ImageWithBpb(Edit edit) {
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Test::FakeImage::BuildFakeNtfsImage();
  edit(*reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data()));
  return image;
}

// Maker of BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
BootImageMaker EditedBpb(Edit edit) {
  return [edit] { return ImageWithBpb(edit); };
}

// Maker of the unmodified fake image.
inline BootImageMaker UntouchedImage() {
  return [] {
    return NTFS_BROWSER_NAMESPACE::Test::FakeImage::BuildFakeNtfsImage();
  };
}

// Maker of the fake image with 0xFF at one byte of the boot sector.
inline BootImageMaker ByteSetAt(size_t offset) {
  return [offset] {
    std::vector<BYTE> image =
        NTFS_BROWSER_NAMESPACE::Test::FakeImage::BuildFakeNtfsImage();
    image.at(offset) = 0xFF;
    return image;
  };
}

// The values a volume decodes from its boot sector.
struct BootGeometry {
  WORD sector_size{0};
  DWORD cluster_size{0};
  ULONGLONG mft_addr{0};
  size_t cluster_buffer_size{0};

  bool operator==(const BootGeometry&) const = default;
};

// What the fake image decodes to. The $MFT sits one cluster in.
inline BootGeometry FakeGeometry() {
  return {.sector_size =
              NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_bytes_per_sector,
          .cluster_size =
              NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_cluster_size,
          .mft_addr =
              NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_cluster_size,
          .cluster_buffer_size =
              NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_cluster_size};
}

// What opening an image logged, and whether the volume came up.
struct Opened {
  bool ok{false};
  BootGeometry geometry;
  std::string log;
};

template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
Opened Open(std::vector<BYTE> image,
            NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  (void)NTFS_BROWSER_NAMESPACE::Test::Support::TakeCapturedLog();
  const NTFS_BROWSER_NAMESPACE::NtfsVolume<S> volume(
      std::make_unique<
          NTFS_BROWSER_NAMESPACE::Test::DiskReaders::MemoryDiskReader>(
          std::move(image)),
      options);
  return {.ok = volume.IsVolumeOK(),
          .geometry = {.sector_size = volume.GetSectorSize(),
                       .cluster_size = volume.GetClusterSize(),
                       .mft_addr = volume.GetMFTAddr(),
                       .cluster_buffer_size = volume.GetClusterBuffer().size()},
          .log = NTFS_BROWSER_NAMESPACE::Test::Support::TakeCapturedLog()};
}

// One boot sector case: an image, the options to open it with and what the
// opening MUST show. Fields left empty are not checked.
struct BootScenario {
  // Name of the section, unique in its test case.
  std::string name;
  BootImageMaker image;
  bool recover{false};
  std::optional<bool> ok;
  std::optional<BootGeometry> geometry;
  // Fragments the log MUST hold.
  std::vector<std::string> contains;
  // Fragments the log MUST NOT hold.
  std::vector<std::string> absent;
  // Fragments the log MUST hold, first to last.
  std::vector<std::string> in_order;
  // A fragment the log MUST hold exactly once.
  std::string once;
};

// Number of non-overlapping occurrences of fragment in text.
inline size_t CountOccurrences(const std::string& text,
                               const std::string& fragment) {
  size_t count = 0;
  for (size_t at = text.find(fragment); at != std::string::npos;
       at = text.find(fragment, at + fragment.size())) {
    ++count;
  }
  return count;
}

// The log holds every fragment, one after the other.
inline void CheckLogOrder(const std::string& log,
                          const std::vector<std::string>& fragments) {
  size_t previous = 0;
  for (const std::string& fragment : fragments) {
    const size_t at = log.find(fragment);
    REQUIRE(at != std::string::npos);
    CHECK(at >= previous);
    previous = at;
  }
}

// The log holds, lacks and orders the fragments the scenario names.
inline void CheckLog(const std::string& log, const BootScenario& scenario) {
  for (const std::string& fragment : scenario.contains) {
    CHECK_THAT(log, Catch::Matchers::ContainsSubstring(fragment));
  }
  for (const std::string& fragment : scenario.absent) {
    CHECK_THAT(log, !Catch::Matchers::ContainsSubstring(fragment));
  }
  CheckLogOrder(log, scenario.in_order);
  if (!scenario.once.empty()) {
    CHECK(CountOccurrences(log, scenario.once) == 1);
  }
}

// Opens the scenario's image and compares the result with its expectations.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
void CheckBootScenario(const BootScenario& scenario) {
  const Opened opened =
      Open<S>(scenario.image(), NTFS_BROWSER_NAMESPACE::VolumeOptions{
                                    .recover_errors = scenario.recover});

  if (scenario.ok) {
    CHECK(opened.ok == *scenario.ok);
  }
  if (scenario.geometry) {
    CHECK(opened.geometry == *scenario.geometry);
  }
  CheckLog(opened.log, scenario);
}

// One section per scenario, named after it.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
void RunBootScenarios(const std::vector<BootScenario>& scenarios) {
  for (const BootScenario& scenario : scenarios) {
    DYNAMIC_SECTION(scenario.name) { CheckBootScenario<S>(scenario); }
  }
}

}  // namespace
