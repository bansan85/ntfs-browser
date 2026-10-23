#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <gsl/narrow>
#include <gsl/util>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "external-mft-support.h"

namespace {

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::DfirMuseumDir;
using NTFS_BROWSER_NAMESPACE::Tests::DfirNtfsDir;
using NTFS_BROWSER_NAMESPACE::Tests::ReadCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::RecoveringOptions;
using NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::ScanStandaloneMft;
using NTFS_BROWSER_NAMESPACE::Tests::StandaloneMftScan;
using NTFS_BROWSER_NAMESPACE::Tests::StrictOptions;
using NTFS_BROWSER_NAMESPACE::Tests::TempFile;

// Records from the start of the $MFT that stay intact: the volume metadata
// files, without which the file does not open as a volume.
constexpr std::size_t protected_records = 16;

// Corruptions of one kind a file takes: single bytes, or records.
constexpr std::size_t corrupted_bytes = 300;
constexpr std::size_t corrupted_records = 12;

// Bytes of a block a fixup array protects, and of a record header.
constexpr std::size_t fixup_block = 512;
constexpr std::size_t header_bytes = 48;

// Bytes of noise written into the attributes of a record.
constexpr std::size_t noise_bytes = 64;

// Kept $MFT files that scan in well under a second.
struct Target {
  std::string_view label;
  std::filesystem::path path;
  std::size_t record_size;
  // Whether record 0 declares fewer clusters than its runs map.
  bool repair;
};

std::vector<Target> Targets() {
  const auto dfir = [](std::string_view name) {
    return DfirNtfsDir() / "test_data" /
           std::u8string(name.begin(), name.end());
  };
  const auto museum = [](std::string_view name) {
    return DfirMuseumDir() / "Windows/NTFSArtifacts/MFT" /
           std::u8string(name.begin(), name.end());
  };
  return {
      {"4k-large", dfir("4k-large.mft"), 4096, false},
      {"orphan", dfir("orphan.mft"), 1024, false},
      {"mapping_pairs_1", dfir("mapping_pairs_1.mft"), 1024, false},
      {"xw", museum("Unknown/xw/$MFT"), 1024, true},
      {"nist-dfr-16", museum("Unknown/NIST/DFR-16/$MFT"), 1024, false},
  };
}

enum class Damage : unsigned char {
  RandomBytes,
  RandomHeaders,
  ZeroedBlocks,
  RandomAttributes
};

std::string_view Name(Damage damage) {
  switch (damage) {
    case Damage::RandomBytes:
      return "random bytes";
    case Damage::RandomHeaders:
      return "random record headers";
    case Damage::ZeroedBlocks:
      return "zeroed blocks";
    case Damage::RandomAttributes:
      return "random attribute bytes";
  }
  return {};
}

// Damages bytes the way damage says, from a generator seeded with seed. The
// same seed gives the same file on every platform: only the raw output of
// std::mt19937 is used, which the standard fixes.
std::vector<BYTE> Corrupt(std::vector<BYTE> bytes, std::size_t record_size,
                          Damage damage, std::uint32_t seed) {
  std::mt19937 engine(seed);
  const std::size_t first = protected_records * record_size;
  const std::size_t records = bytes.size() / record_size;
  const auto some_record = [&] {
    return protected_records + (engine() % (records - protected_records));
  };
  const auto random_byte = [&] { return gsl::narrow_cast<BYTE>(engine()); };

  switch (damage) {
    case Damage::RandomBytes:
      for (std::size_t index = 0; index < corrupted_bytes; index++) {
        bytes.at(first + (engine() % (bytes.size() - first))) = random_byte();
      }
      break;
    case Damage::RandomHeaders:
      for (std::size_t index = 0; index < corrupted_records; index++) {
        const std::size_t start = some_record() * record_size;
        for (std::size_t offset = 0; offset < header_bytes; offset++) {
          bytes.at(start + offset) = random_byte();
        }
      }
      break;
    case Damage::ZeroedBlocks:
      for (std::size_t index = 0; index < corrupted_records; index++) {
        const std::size_t start =
            (some_record() * record_size) +
            (engine() % (record_size / fixup_block) * fixup_block);
        std::fill_n(bytes.begin() + gsl::narrow<std::ptrdiff_t>(start),
                    fixup_block, BYTE{0});
      }
      break;
    case Damage::RandomAttributes:
      for (std::size_t index = 0; index < corrupted_records; index++) {
        const std::size_t start =
            (some_record() * record_size) + header_bytes +
            (engine() % (record_size - header_bytes - noise_bytes));
        for (std::size_t offset = 0; offset < noise_bytes; offset++) {
          bytes.at(start + offset) = random_byte();
        }
      }
      break;
  }
  return bytes;
}

std::set<ULONGLONG> Records(const Mft::MftTree& tree) {
  std::set<ULONGLONG> records;
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    records.insert(entry.record);
  }
  return records;
}

// The counts of a scan that two scans of one file MUST share.
auto Counters(const Mft::MftTree::ScanStats& stats) {
  return std::tie(stats.slots, stats.in_use, stats.deleted, stats.extensions,
                  stats.unreadable, stats.damaged);
}

// Whether two scans of one file kept the same records and counted alike.
bool SameScan(const Mft::MftTree& left, const Mft::MftTree& right) {
  return Counters(left.Stats()) == Counters(right.Stats()) &&
         Records(left) == Records(right);
}

}  // namespace

TEST_CASE("A corrupted real $MFT scans to the end in every mode and strategy",
          "[dfir-corpora][integration][mft][corruption]") {
  const std::vector<Target> targets = Targets();
  const Target& target = targets.at(GENERATE(range(0, 5)));
  const Damage damage =
      GENERATE(Damage::RandomBytes, Damage::RandomHeaders, Damage::ZeroedBlocks,
               Damage::RandomAttributes);
  const std::uint32_t seed = GENERATE(1U, 2U, 3U, 4U, 5U);
  CAPTURE(target.label, Name(damage), seed);

  const std::filesystem::path original = RequireCorpusFile(target.path);
  const std::vector<BYTE> bytes =
      Corrupt(ReadCorpusFile(original), target.record_size, damage, seed);
  const TempFile file(bytes);

  for (const VolumeOptions& options : {StrictOptions(), RecoveringOptions()}) {
    CAPTURE(options.recover_errors);
    const StandaloneMftScan no_cache = ScanStandaloneMft(
        file.Path(), Cache::Strategy::NoCache, options, target.repair);
    const StandaloneMftScan full_cache = ScanStandaloneMft(
        file.Path(), Cache::Strategy::FullCache, options, target.repair);

    const Mft::MftTree::ScanStats& stats = no_cache.tree.Stats();
    CHECK(stats.slots == bytes.size() / target.record_size);
    CHECK(stats.in_use + stats.deleted + stats.extensions + stats.unreadable ==
          stats.slots);
    CHECK(no_cache.tree.Entries().size() <= stats.in_use + stats.deleted);
    CHECK(SameScan(no_cache.tree, full_cache.tree));
  }
}
