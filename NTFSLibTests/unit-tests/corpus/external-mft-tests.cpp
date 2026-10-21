#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <gsl/narrow>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "attr/vol-name.h"
#include "corpus-test-support.h"
#include "external-mft-support.h"
#include "io/file-record-attr-types.h"
#include "md5-test-support.h"
#include "partition-disk-reader.h"
#include "standalone-mft-disk-reader.h"
#include "utf/utf.h"
#include "volume/volume-geometry.h"

namespace {

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Tests::DfirMuseumDir;
using NTFS_BROWSER_NAMESPACE::Tests::DfirNtfsDir;
using NTFS_BROWSER_NAMESPACE::Tests::EntryUtcSeconds;
using NTFS_BROWSER_NAMESPACE::Tests::EntryUtcTicks;
using NTFS_BROWSER_NAMESPACE::Tests::FlsLine;
using NTFS_BROWSER_NAMESPACE::Tests::ListingPath;
using NTFS_BROWSER_NAMESPACE::Tests::OpenStandaloneMftVolume;
using NTFS_BROWSER_NAMESPACE::Tests::ParseFlsListing;
using NTFS_BROWSER_NAMESPACE::Tests::RecoveringOptions;
using NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::ScanStandaloneMft;
using NTFS_BROWSER_NAMESPACE::Tests::StandaloneMftDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::StandaloneMftScan;
using NTFS_BROWSER_NAMESPACE::Tests::StrictOptions;
using NTFS_BROWSER_NAMESPACE::Tests::TempFile;
using NTFS_BROWSER_NAMESPACE::Tests::UtcTicks;

// Which kept project holds a fixture.
enum class Project : unsigned char { DfirNtfs, DfirMuseum };

// Where the museum keeps its $MFT files, under its root.
constexpr std::string_view museum_mft = "Windows/NTFSArtifacts/MFT/";

// One kept $MFT fixture and what an independent reader (a Python script that
// reads the file's records and their fixups alone, never this library) found in
// it. A slot counts as unreadable when it has no FILE signature (BAAD and zeros
// included) or fails its fixup check. A freed record is a base record without
// the in-use flag. Directories are base records in use with the directory flag.
// damaged_strict and damaged_recovering are the library's own answer, pinned as
// it is today: ScanStats::damaged in strict and in recovering mode. salvaged is
// the independent reader's count of freed records that fail the fixup check on
// one sector only: a recovering scan keeps them as deleted records, and a
// strict one counts them unreadable.
struct MftFixture {
  std::string_view label;
  Project project;
  std::string_view relative_path;
  std::uintmax_t size;
  DWORD record_size;
  DWORD cluster_size;
  ULONGLONG slots;
  ULONGLONG in_use;
  ULONGLONG deleted;
  ULONGLONG extensions;
  ULONGLONG unreadable;
  ULONGLONG directories;
  ULONGLONG damaged_strict;
  ULONGLONG damaged_recovering;
  ULONGLONG salvaged;
};

// The fixtures that scan in seconds.
constexpr std::array<MftFixture, 8> small_fixtures = {{
    {"nist-hacking-case", Project::DfirNtfs, "test_data/nist-hacking-case.mft",
     12'600'320, 1024, 512, 12305, 12180, 3, 121, 1, 767, 0, 0, 0},
    {"4k-large", Project::DfirNtfs, "test_data/4k-large.mft", 1048576, 4096,
     4096, 256, 36, 0, 0, 220, 9, 0, 0, 0},
    {"orphan", Project::DfirNtfs, "test_data/orphan.mft", 262144, 1024, 4096,
     256, 36, 4, 0, 216, 11, 0, 0, 0},
    {"deleted", Project::DfirNtfs, "test_data/deleted.mft", 262144, 1024, 4096,
     256, 35, 6, 0, 215, 9, 0, 0, 0},
    {"unicode", Project::DfirNtfs, "test_data/unicode.mft", 262144, 1024, 4096,
     256, 36, 0, 0, 220, 10, 0, 0, 0},
    {"mapping_pairs_1", Project::DfirNtfs, "test_data/mapping_pairs_1.mft",
     262144, 1024, 4096, 256, 41, 2, 0, 213, 10, 0, 0, 0},
    {"xw", Project::DfirMuseum, "Unknown/xw/$MFT", 643072, 1024, 2048, 628, 466,
     149, 1, 12, 31, 13, 16, 10},
    {"nist-dfr-16", Project::DfirMuseum, "Unknown/NIST/DFR-16/$MFT", 157696,
     1024, 512, 154, 117, 0, 0, 37, 50, 0, 0, 0},
}};

// The fixtures of 50 MB and more: a scan takes seconds in a Release build and
// up to a minute in a Debug one.
constexpr std::array<MftFixture, 4> large_fixtures = {{
    {"tdungan", Project::DfirMuseum, "Unknown/tdungan/$MFT", 53'575'680, 1024,
     4096, 52320, 42754, 9443, 123, 0, 4939, 6, 0, 0},
    {"stolen-szechuan-2012r2", Project::DfirMuseum,
     "Win2012R2/StolenSzechuan/$MFT", 89'391'104, 1024, 4096, 87296, 85986, 0,
     1154, 156, 20162, 5, 0, 0},
    {"rathbun-win10", Project::DfirMuseum, "Win10/RathbunVM/$MFT", 121'634'816,
     1024, 4096, 118784, 104218, 12107, 2332, 127, 29288, 76, 0, 0},
    {"rathbun-win11", Project::DfirMuseum, "Win11/RathbunVM/$MFT", 104'857'600,
     1024, 4096, 102400, 98739, 0, 3485, 176, 25147, 92, 0, 0},
}};

std::filesystem::path PathOf(const MftFixture& fixture) {
  if (fixture.project == Project::DfirNtfs) {
    return DfirNtfsDir() / std::u8string(fixture.relative_path.begin(),
                                         fixture.relative_path.end());
  }
  const std::string full =
      std::string(museum_mft) + std::string(fixture.relative_path);
  return DfirMuseumDir() / std::u8string(full.begin(), full.end());
}

// Whether the fixture's record 0 needs StandaloneMftDiskReader's repair to
// open in strict mode: xw declares 298 clusters for 314 clusters of runs.
bool NeedsRepair(const MftFixture& fixture) { return fixture.label == "xw"; }

std::filesystem::path Dfir(std::string_view name) {
  return DfirNtfsDir() / "test_data" / std::u8string(name.begin(), name.end());
}

std::filesystem::path Museum(std::string_view relative) {
  const std::string full = std::string(museum_mft) + std::string(relative);
  return DfirMuseumDir() / std::u8string(full.begin(), full.end());
}

// A time in whole microseconds, the precision the fixtures' documentation
// keeps. Entry times are local on Windows: EntryUtcTicks() undoes it.
std::uint64_t EntryUtcMicros(const FILETIME& local_time) {
  constexpr std::uint64_t ticks_per_microsecond = 10;
  return EntryUtcTicks(local_time) / ticks_per_microsecond;
}

std::uint64_t UtcMicros(int year, unsigned month, unsigned day, unsigned hour,
                        unsigned minute, unsigned second,
                        unsigned microsecond) {
  constexpr std::uint64_t ticks_per_microsecond = 10;
  return UtcTicks(year, month, day, hour, minute, second, microsecond) /
         ticks_per_microsecond;
}

// Names the first field two entries disagree on. Empty when they are equal.
// strict_left is true when left comes from a strict scan and right from a
// recovering one: a strict scan drops the damaged parent of a name, so the
// name may be linked to it only on the right.
std::string FirstDifference(const Mft::MftTree::Entry& left,
                            const Mft::MftTree::Entry& right,
                            bool strict_left = false) {
  using NTFS_BROWSER_NAMESPACE::Tests::FileTimeToTicks;
  if (left.record != right.record) {
    return "record";
  }
  if (left.sequence != right.sequence) {
    return "sequence";
  }
  if (left.in_use != right.in_use || left.directory != right.directory) {
    return "flags";
  }
  if (left.names.size() != right.names.size()) {
    return "name count";
  }
  for (std::size_t index = 0; index < left.names.size(); index++) {
    const Mft::MftTree::Name& lname = left.names.at(index);
    const Mft::MftTree::Name& rname = right.names.at(index);
    const bool link_differs =
        lname.parent_valid != rname.parent_valid &&
        !(strict_left && !lname.parent_valid && rname.parent_valid);
    if (lname.name != rname.name ||
        lname.parent_record != rname.parent_record ||
        lname.parent_sequence != rname.parent_sequence ||
        lname.dos_only != rname.dos_only || link_differs) {
      return "name " + std::to_string(index);
    }
  }
  if (left.size != right.size || left.allocated_size != right.allocated_size) {
    return "size";
  }
  if (FileTimeToTicks(left.create_time) != FileTimeToTicks(right.create_time) ||
      FileTimeToTicks(left.write_time) != FileTimeToTicks(right.write_time) ||
      FileTimeToTicks(left.change_time) != FileTimeToTicks(right.change_time) ||
      FileTimeToTicks(left.access_time) != FileTimeToTicks(right.access_time)) {
    return "times";
  }
  if (left.read_only != right.read_only || left.hidden != right.hidden ||
      left.system != right.system || left.archive != right.archive ||
      left.compressed != right.compressed ||
      left.encrypted != right.encrypted || left.sparse != right.sparse) {
    return "attribute flags";
  }
  if (left.data_state != right.data_state) {
    return "data state";
  }
  if (left.usn.has_value() != right.usn.has_value()) {
    return "usn";
  }
  return {};
}

// Names the first thing two scans disagree on. Empty when they are the same
// kept records with the same counters.
std::string FirstDifference(const Mft::MftTree& left,
                            const Mft::MftTree& right) {
  const Mft::MftTree::ScanStats& lstats = left.Stats();
  const Mft::MftTree::ScanStats& rstats = right.Stats();
  if (lstats.slots != rstats.slots || lstats.in_use != rstats.in_use ||
      lstats.deleted != rstats.deleted ||
      lstats.extensions != rstats.extensions ||
      lstats.unreadable != rstats.unreadable ||
      lstats.skipped != rstats.skipped || lstats.damaged != rstats.damaged ||
      lstats.unreachable != rstats.unreachable ||
      lstats.complete != rstats.complete ||
      lstats.data_intact != rstats.data_intact ||
      lstats.data_reallocated != rstats.data_reallocated ||
      lstats.data_unknown != rstats.data_unknown ||
      lstats.data_bad != rstats.data_bad) {
    return "stats";
  }
  if (left.Entries().size() != right.Entries().size()) {
    return "entry count";
  }
  for (std::size_t index = 0; index < left.Entries().size(); index++) {
    const std::string field =
        FirstDifference(left.Entries().at(index), right.Entries().at(index));
    if (!field.empty()) {
      return "record " + std::to_string(left.Entries().at(index).record) +
             ": " + field;
    }
  }
  return {};
}

// How many kept entries are in use base directories.
ULONGLONG CountDirectories(const Mft::MftTree& tree) {
  return gsl::narrow<ULONGLONG>(std::ranges::count_if(
      tree.Entries(), [](const Mft::MftTree::Entry& entry) {
        return entry.in_use && entry.directory;
      }));
}

// The first `count` bytes of file.
std::vector<BYTE> Head(const std::filesystem::path& file, std::size_t count) {
  std::vector<BYTE> bytes = NTFS_BROWSER_NAMESPACE::Tests::ReadCorpusFile(file);
  bytes.resize(std::min(count, bytes.size()));
  return bytes;
}

// Writes value at offset of bytes, little endian.
template <typename T>
void Put(std::vector<BYTE>& bytes, std::size_t offset, T value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(value));
}

// Offset of the update sequence array's own offset in a file record header.
constexpr std::size_t header_offset_of_us = 0x04;

// Offset of its size, in words.
constexpr std::size_t header_size_of_us = 0x06;

// Offset of the offset of the first attribute.
constexpr std::size_t header_offset_of_attr = 0x14;

// Offset of the record's allocated size, which is its size on the volume.
constexpr std::size_t header_alloc_size = 0x1C;

// Where the synthetic records put their update sequence array: right after
// the 48-byte header.
constexpr std::size_t update_sequence_at = 0x30;

// Where their first attribute begins: after the 3 words of the array of a
// 1024-byte record, rounded up to 8 bytes.
constexpr std::size_t first_attr_at = 0x38;

// A file record header of record_size bytes, with its update sequence array
// and an end-of-attributes marker, and no attribute. alloc_size is what the
// header declares, whatever record_size is.
std::vector<BYTE> MakeEmptyRecord(std::size_t record_size,
                                  std::uint32_t alloc_size) {
  std::vector<BYTE> bytes(record_size);
  Put<std::uint32_t>(bytes, 0, 0x454C4946);
  Put<std::uint16_t>(bytes, header_offset_of_us, update_sequence_at);
  Put<std::uint16_t>(bytes, header_size_of_us,
                     gsl::narrow<std::uint16_t>(record_size / 512 + 1));
  Put<std::uint16_t>(bytes, header_offset_of_attr, first_attr_at);
  Put<std::uint32_t>(bytes, header_alloc_size, alloc_size);
  // The update sequence number is 1, and each block end repeats it.
  Put<std::uint16_t>(bytes, update_sequence_at, 1);
  for (std::size_t block = 1; block <= record_size / 512; block++) {
    Put<std::uint16_t>(bytes, block * 512 - 2, 1);
  }
  Put<std::uint32_t>(bytes, first_attr_at, 0xFFFFFFFF);
  return bytes;
}

// Cluster size of the $MFT the synthetic record 0 describes: a $MFT of one
// 1024-byte record, in two 512-byte clusters.
constexpr DWORD synthetic_cluster_size = 512;

// Cluster where it begins: 16 KiB, beyond the 4 KiB boot area.
constexpr ULONGLONG synthetic_lcn = 32;

// A 1024-byte record 0 whose only attribute is a non-resident unnamed $DATA of
// two 512-byte clusters at synthetic_lcn.
std::vector<BYTE> MakeSyntheticRecord0() {
  std::vector<BYTE> bytes = MakeEmptyRecord(1024, 1024);
  constexpr std::size_t attr_size = 0x48;
  Put<std::uint32_t>(bytes, first_attr_at, 0x80);
  Put<std::uint32_t>(bytes, first_attr_at + 4, attr_size);
  // Non-resident, unnamed, run list at 0x40.
  Put<std::uint8_t>(bytes, first_attr_at + 8, 1);
  Put<std::uint64_t>(bytes, first_attr_at + 0x10, 0);
  Put<std::uint64_t>(bytes, first_attr_at + 0x18, 1);
  Put<std::uint16_t>(bytes, first_attr_at + 0x20, 0x40);
  Put<std::uint64_t>(bytes, first_attr_at + 0x28, 1024);
  Put<std::uint64_t>(bytes, first_attr_at + 0x30, 1024);
  Put<std::uint64_t>(bytes, first_attr_at + 0x38, 1024);
  // One run: a header byte with a 1-byte length and a 1-byte offset, 2
  // clusters, cluster synthetic_lcn, then the terminator.
  Put<std::uint8_t>(bytes, first_attr_at + 0x40, 0x11);
  Put<std::uint8_t>(bytes, first_attr_at + 0x41, 2);
  Put<std::uint8_t>(bytes, first_attr_at + 0x42,
                    gsl::narrow<std::uint8_t>(synthetic_lcn));
  Put<std::uint32_t>(bytes, first_attr_at + attr_size, 0xFFFFFFFF);
  return bytes;
}

// What every field of a listing line says, checked against tree. Collects the
// first few differences, since a listing holds thousands of lines.
struct ListingReport {
  std::size_t checked{0};
  std::vector<std::string> differences;

  void Note(const std::string& message) {
    constexpr std::size_t max_notes = 8;
    if (differences.size() < max_notes) {
      differences.push_back(message);
    }
  }
};

// Compares every line of a Sleuth Kit listing with the tree. The tree holds
// the files in use. Conventions of the listing:
//   - the `V/V` line is the virtual $OrphanFiles directory, in no record;
//   - times are the four of $STANDARD_INFORMATION, in the order modification,
//     access, MFT change, creation, with a whole-second precision;
//   - the size of an `r/r` line of the unnamed $DATA is the stream's real
//     size, and the size of a `d/d` line is its index's, so it is not compared;
//   - a named stream is a line of its own (`file:stream`), and a directory
//     with only named streams has only those lines;
//   - a name that is only a DOS 8.3 alias is never listed;
//   - the root directory is never listed;
//   - the `*` of a deleted file is not in any of the kept listings.
ListingReport CompareWithListing(const Mft::MftTree& tree,
                                 const std::vector<FlsLine>& lines) {
  ListingReport report;
  std::set<std::pair<ULONGLONG, std::string>> listed;
  for (const FlsLine& line : lines) {
    if (line.kind == "V/V") {
      continue;
    }
    listed.emplace(line.record, line.path);
    const std::string where =
        std::to_string(line.record) + " " + line.path + ": ";

    const Mft::MftTree::Entry* entry = tree.Find(line.record);
    if (entry == nullptr) {
      report.Note(where + "no entry");
      continue;
    }
    bool named = false;
    for (std::size_t index = 0; index < entry->names.size(); index++) {
      named =
          named || (!entry->names.at(index).dos_only &&
                    ListingPath(tree.GetPath(line.record, index)) == line.path);
    }
    if (!named) {
      report.Note(where + "no such name");
      continue;
    }
    report.checked++;

    const std::array<std::uint64_t, 4> times = {
        EntryUtcSeconds(entry->write_time), EntryUtcSeconds(entry->access_time),
        EntryUtcSeconds(entry->change_time),
        EntryUtcSeconds(entry->create_time)};
    if (times != line.times) {
      report.Note(where + "times");
    }
    if (line.stream.empty() && entry->directory != (line.kind == "d/d")) {
      report.Note(where + "kind");
    }
    if (line.stream.empty() && line.kind == "r/r" &&
        line.attribute_type == 128 && entry->size != line.size) {
      report.Note(where + "size " + std::to_string(entry->size) + " vs " +
                  std::to_string(line.size));
    }
  }

  const ULONGLONG root = static_cast<ULONGLONG>(Mft::Idx::Root);
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    if (entry.record == root) {
      continue;
    }
    for (std::size_t index = 0; index < entry.names.size(); index++) {
      if (!entry.names.at(index).dos_only &&
          !listed.contains(
              {entry.record, ListingPath(tree.GetPath(entry.record, index))})) {
        report.Note(std::to_string(entry.record) + ": allocated but unlisted");
      }
    }
  }
  return report;
}

// The Sleuth Kit listing shipped with a kept $MFT, and how many lines it has.
struct ListingFixture {
  std::string_view mft;
  std::string_view listing;
  std::size_t lines;
};

constexpr std::array<ListingFixture, 4> listing_fixtures = {{
    {"nist-hacking-case.mft", "nist-hacking-case.fls", 12180},
    {"4k-large.mft", "4k-large.fls", 41},
    {"orphan.mft", "orphan.fls", 41},
    {"unicode.mft", "unicode.fls", 41},
}};

// Largest read the NoCache scan of a volume makes: its 4 MiB $MFT block.
constexpr std::uint64_t max_scan_read = std::uint64_t{4} * 1024 * 1024;

// Ceiling of the FullCache of every scan, as external-mft-support.cpp sets it.
constexpr std::uint64_t cache_ceiling = std::uint64_t{8} * 1024 * 1024;

// Checks what only one fixture has: the names, paths and times its own
// documentation gives, from the scans that found them.
void CheckFixtureFacts(const MftFixture& fixture,
                       const StandaloneMftScan& strict,
                       const StandaloneMftScan& recovering) {
  if (fixture.label == "nist-hacking-case") {
    // Record 16 is the BAAD one: no scan keeps it.
    CHECK(strict.tree.Find(16) == nullptr);
    CHECK(recovering.tree.Find(16) == nullptr);
  } else if (fixture.label == "xw") {
    // BAAD records 500 and 565 are kept by no scan. The ten FILE records
    // whose fixup check fails on one sector are dropped by strict mode and
    // kept by recovering one, record 179 (a freed file with intact
    // attributes) among them.
    for (const ULONGLONG record : {500ULL, 565ULL}) {
      CAPTURE(record);
      CHECK(strict.tree.Find(record) == nullptr);
      CHECK(recovering.tree.Find(record) == nullptr);
    }
    for (const ULONGLONG record : {179ULL, 540ULL, 546ULL, 551ULL, 554ULL,
                                   566ULL, 567ULL, 568ULL, 586ULL, 616ULL}) {
      CAPTURE(record);
      CHECK(strict.tree.Find(record) == nullptr);
      CHECK(recovering.tree.Find(record) != nullptr);
    }
    // Record 432 is an extension of record 485: an extension is no entry.
    CHECK(strict.tree.Find(432) == nullptr);
    CHECK(recovering.tree.Find(432) == nullptr);
    // Records with no end marker, or with one past the used size, are kept
    // when recovering and dropped by strict mode.
    for (const ULONGLONG record : {180ULL, 202ULL, 442ULL, 485ULL, 612ULL}) {
      CAPTURE(record);
      CHECK(strict.tree.Find(record) == nullptr);
      CHECK(recovering.tree.Find(record) != nullptr);
    }
  } else if (fixture.label == "tdungan") {
    // Times are those of the raw FILETIMEs, in UTC. The documented values stop
    // at the microsecond: the last digit of a tick is dropped.
    const Mft::MftTree::Entry* root = recovering.tree.Find(5);
    REQUIRE(root != nullptr);
    CHECK(EntryUtcMicros(root->create_time) ==
          UtcMicros(2010, 11, 10, 17, 38, 1, 62500));
    CHECK(EntryUtcMicros(root->write_time) ==
          UtcMicros(2012, 4, 6, 17, 27, 41, 610682));
    CHECK(EntryUtcMicros(root->access_time) ==
          UtcMicros(2012, 4, 7, 2, 33, 13, 583147));

    const Mft::MftTree::Entry* log = recovering.tree.Find(329);
    REQUIRE(log != nullptr);
    CHECK(recovering.tree.GetPath(329) ==
          L"\\Documents and Settings\\RSydow\\Local Settings\\Temp\\vmmsi.log");
    CHECK(EntryUtcMicros(log->create_time) ==
          UtcMicros(2011, 7, 6, 1, 4, 45, 406250));
    CHECK(EntryUtcMicros(log->write_time) ==
          UtcMicros(2011, 7, 5, 18, 6, 7, 722625));

    CHECK(recovering.tree.GetPath(40000) ==
          L"\\WINDOWS\\$NtServicePackUninstall$\\reg00099");
    // Record 17863 has the NTFS 3.0 short header (fixup array at 0x2A). It is
    // an extension of record 19866: no entry of its own, and its base is kept.
    CHECK(strict.tree.Find(17863) == nullptr);
    CHECK(recovering.tree.Find(17863) == nullptr);
    CHECK(recovering.tree.Find(19866) != nullptr);
  }
}

// Scans fixture four times (strict and recovering, NoCache and FullCache) and
// checks what every kept $MFT must show.
void CheckFixture(const MftFixture& fixture) {
  CAPTURE(fixture.label);
  const std::filesystem::path file = RequireCorpusFile(PathOf(fixture));
  REQUIRE(std::filesystem::file_size(file) == fixture.size);
  const bool repair = NeedsRepair(fixture);

  const StandaloneMftScan strict_no_cache = ScanStandaloneMft(
      file, Cache::Strategy::NoCache, StrictOptions(), repair);
  const StandaloneMftScan strict_full_cache = ScanStandaloneMft(
      file, Cache::Strategy::FullCache, StrictOptions(), repair);
  const StandaloneMftScan recover_no_cache = ScanStandaloneMft(
      file, Cache::Strategy::NoCache, RecoveringOptions(), repair);
  const StandaloneMftScan recover_full_cache = ScanStandaloneMft(
      file, Cache::Strategy::FullCache, RecoveringOptions(), repair);

  // Both sizes come from record 0.
  CHECK(strict_no_cache.record_size == fixture.record_size);
  CHECK(strict_no_cache.cluster_size == fixture.cluster_size);

  // The cache strategy changes nothing in what a scan returns.
  CHECK(FirstDifference(strict_no_cache.tree, strict_full_cache.tree).empty());
  CHECK(
      FirstDifference(recover_no_cache.tree, recover_full_cache.tree).empty());

  // The counts equal the independent reader's. Recovering keeps the freed
  // records that fail the fixup check on one sector only.
  for (const StandaloneMftScan* scan : {&strict_no_cache, &recover_no_cache}) {
    const ULONGLONG salvaged = scan == &recover_no_cache ? fixture.salvaged : 0;
    const Mft::MftTree::ScanStats& stats = scan->tree.Stats();
    CHECK(stats.complete);
    CHECK(stats.slots == fixture.slots);
    CHECK(stats.in_use == fixture.in_use);
    CHECK(stats.deleted == fixture.deleted + salvaged);
    CHECK(stats.extensions == fixture.extensions);
    CHECK(stats.unreadable == fixture.unreadable - salvaged);
    CHECK(stats.skipped == 0);
    CHECK(stats.in_use + stats.deleted + stats.extensions + stats.unreadable ==
          stats.slots);
  }
  CHECK(CountDirectories(recover_no_cache.tree) == fixture.directories);

  // Strict drops a damaged record, recovering keeps what parses. Recovering
  // can salvage a record whose attribute list or run list is cut, which a
  // strict scan counts as damaged: it counts fewer. A standalone $MFT lacks
  // the clusters of every non-resident $ATTRIBUTE_LIST, so strict mode counts
  // those records too. Recovering also counts the records it salvages with a
  // failed fixup, which strict mode counts unreadable.
  const Mft::MftTree::ScanStats& strict = strict_no_cache.tree.Stats();
  const Mft::MftTree::ScanStats& recover = recover_no_cache.tree.Stats();
  CHECK(strict.damaged == fixture.damaged_strict);
  CHECK(recover.damaged == fixture.damaged_recovering);
  CHECK(strict_no_cache.tree.Entries().size() + strict.damaged ==
        strict.in_use + strict.deleted);
  CHECK(recover_no_cache.tree.Entries().size() ==
        recover.in_use + recover.deleted);

  // A record strict mode keeps is the same in recovering mode, except that a
  // name whose parent strict mode dropped is not linked to it.
  std::size_t different = 0;
  for (const Mft::MftTree::Entry& entry : strict_no_cache.tree.Entries()) {
    const Mft::MftTree::Entry* other = recover_no_cache.tree.Find(entry.record);
    if (other == nullptr || !FirstDifference(entry, *other, true).empty()) {
      different++;
    }
  }
  CHECK(different == 0);

  // Neither scan holds the file: NoCache reads it in blocks of 4 MiB, and the
  // FullCache keeps what its ceiling allows.
  for (const StandaloneMftScan* scan :
       {&strict_no_cache, &recover_no_cache, &strict_full_cache,
        &recover_full_cache}) {
    CHECK(scan->largest_read <= max_scan_read);
  }
  CHECK(strict_no_cache.held_bytes == 0);
  CHECK(strict_full_cache.held_bytes <= cache_ceiling);
  CHECK(recover_full_cache.held_bytes <= cache_ceiling);

  CheckFixtureFacts(fixture, strict_no_cache, recover_no_cache);
}

}  // namespace

TEST_CASE("Small standalone $MFT files scan alike in every mode and strategy",
          "[dfir-corpora][integration][mft]") {
  CheckFixture(GENERATE(from_range(small_fixtures)));
}

TEST_CASE("Large standalone $MFT files scan alike in every mode and strategy",
          "[dfir-corpora][integration][mft][slow]") {
  CheckFixture(GENERATE(from_range(large_fixtures)));
}

TEST_CASE("A standalone $MFT scans with the default volume options",
          "[dfir-corpora][integration][mft]") {
  const MftFixture fixture = GENERATE(from_range(small_fixtures));
  CAPTURE(fixture.label);
  const std::filesystem::path file = RequireCorpusFile(PathOf(fixture));

  const StandaloneMftScan scan = ScanStandaloneMft(
      file, Cache::Strategy::NoCache, VolumeOptions{}, NeedsRepair(fixture));
  const Mft::MftTree::ScanStats& stats = scan.tree.Stats();

  // The bitmap of $MFT is not in the file, so the scan reads every slot.
  CHECK(stats.slots == fixture.slots);
  CHECK(stats.skipped == 0);
  CHECK(stats.deleted == fixture.deleted);
  CHECK(std::ranges::all_of(
      scan.tree.Entries(),
      [](const Mft::MftTree::Entry& entry) { return entry.in_use; }));
  CHECK(scan.tree.Entries().size() + stats.damaged == fixture.in_use);
}

TEST_CASE("A standalone $MFT agrees with its Sleuth Kit listing",
          "[dfir-corpora][integration][mft][fls]") {
  const ListingFixture fixture = GENERATE(from_range(listing_fixtures));
  CAPTURE(fixture.mft);
  const std::filesystem::path file = RequireCorpusFile(Dfir(fixture.mft));
  const std::vector<FlsLine> lines =
      ParseFlsListing(RequireCorpusFile(Dfir(fixture.listing)));
  REQUIRE(lines.size() == fixture.lines);
  // $OrphanFiles is the last line.
  CHECK(lines.back().kind == "V/V");
  CHECK(lines.front().kind != "V/V");

  // The listing holds the files in use, which both modes find alike.
  for (const VolumeOptions& options :
       {StrictOptions(false), RecoveringOptions(false)}) {
    CAPTURE(options.recover_errors);
    const StandaloneMftScan scan =
        ScanStandaloneMft(file, Cache::Strategy::NoCache, options);
    const ListingReport report = CompareWithListing(scan.tree, lines);
    INFO("first differences: "
         << ::Catch::Detail::stringify(report.differences));
    CHECK(report.differences.empty());
    // Every line but $OrphanFiles was found.
    CHECK(report.checked == lines.size() - 1);
  }
}

TEST_CASE("The boot sector of a standalone $MFT parses to record 0's geometry",
          "[dfir-corpora][integration][mft]") {
  const MftFixture fixture = GENERATE(from_range(small_fixtures));
  CAPTURE(fixture.label);
  const std::filesystem::path file = RequireCorpusFile(PathOf(fixture));

  StandaloneMftDiskReader reader;
  reader.SetRepairDeclaredExtent(NeedsRepair(fixture));
  REQUIRE(reader.Open(file.wstring()));
  CHECK(reader.FileRecordSize() == fixture.record_size);
  CHECK(reader.ClusterSize() == fixture.cluster_size);
  CHECK(reader.FileSize() == fixture.size);

  std::vector<BYTE> boot(512);
  LARGE_INTEGER addr{};
  REQUIRE(reader.ReadInto(addr, boot));
  const NTFS_BROWSER_NAMESPACE::Volume::BootSectorParse parse =
      NTFS_BROWSER_NAMESPACE::Volume::ParseBootSector(boot, false);
  REQUIRE(parse.ok);
  CHECK(parse.geometry.sector_size == 512);
  CHECK(parse.geometry.cluster_size == fixture.cluster_size);
  CHECK(parse.geometry.file_record_size == fixture.record_size);
  CHECK(parse.geometry.index_block_size == 4096);
  CHECK_FALSE(parse.geometry.mft_mirr_usable);
  // The volume ends with the last cluster $MFT maps.
  CHECK(reader.Size() == parse.geometry.cluster_count * fixture.cluster_size);
}

TEST_CASE("A synthetic record 0 opens as a standalone $MFT",
          "[standalone-mft][synthetic]") {
  const std::vector<BYTE> record = MakeSyntheticRecord0();
  const TempFile file(record);

  StandaloneMftDiskReader reader;
  REQUIRE(reader.Open(file.Path().wstring()));
  CHECK(reader.Failure().empty());
  CHECK(reader.FileRecordSize() == 1024);
  CHECK(reader.ClusterSize() == synthetic_cluster_size);
  CHECK(reader.FileSize() == 1024);
  CHECK(reader.Size() == (synthetic_lcn + 2) * synthetic_cluster_size);

  SECTION("byte 0 is a boot sector") {
    std::vector<BYTE> boot(512);
    LARGE_INTEGER addr{};
    REQUIRE(reader.ReadInto(addr, boot));
    const auto parse =
        NTFS_BROWSER_NAMESPACE::Volume::ParseBootSector(boot, false);
    REQUIRE(parse.ok);
    CHECK(parse.geometry.cluster_size == synthetic_cluster_size);
    CHECK(parse.geometry.file_record_size == 1024);
    CHECK(parse.geometry.mft_addr == synthetic_lcn * synthetic_cluster_size);
  }

  SECTION("the clusters of $DATA are the bytes of the file") {
    std::vector<BYTE> read(1024);
    LARGE_INTEGER addr{.QuadPart = gsl::narrow<LONGLONG>(
                           synthetic_lcn * synthetic_cluster_size)};
    REQUIRE(reader.ReadInto(addr, read));
    CHECK(read == record);

    // A read may start inside a run and end in it.
    std::vector<BYTE> middle(100);
    addr.QuadPart += 700;
    REQUIRE(reader.ReadInto(addr, middle));
    CHECK(std::equal(middle.begin(), middle.end(), record.begin() + 700));
  }

  SECTION("any other byte fails to read") {
    std::vector<BYTE> read(16);
    // Past the end of the last run.
    LARGE_INTEGER addr{.QuadPart = gsl::narrow<LONGLONG>(
                           (synthetic_lcn + 2) * synthetic_cluster_size)};
    CHECK_FALSE(reader.ReadInto(addr, read));
    // A read that starts in the run and ends outside it.
    addr.QuadPart -= 8;
    CHECK_FALSE(reader.ReadInto(addr, read));
    // Between the boot area and the run.
    addr.QuadPart = 8192;
    CHECK_FALSE(reader.ReadInto(addr, read));
    // Inside the boot area, past the boot sector: zeros.
    addr.QuadPart = 1024;
    CHECK(reader.ReadInto(addr, read));
    CHECK(std::ranges::all_of(read, [](BYTE byte) { return byte == 0; }));
    addr.QuadPart = -1;
    CHECK_FALSE(reader.ReadInto(addr, read));
  }
}

TEST_CASE("A file that is not a usable first $MFT record is refused",
          "[standalone-mft][synthetic]") {
  struct Case {
    std::string_view label;
    std::vector<BYTE> bytes;
  };

  std::vector<BYTE> bad_sequence = MakeSyntheticRecord0();
  Put<std::uint16_t>(bad_sequence, 510, 0x7777);
  std::vector<BYTE> short_array = MakeSyntheticRecord0();
  Put<std::uint16_t>(short_array, header_size_of_us, 2);
  std::vector<BYTE> array_out_of_record = MakeSyntheticRecord0();
  Put<std::uint16_t>(array_out_of_record, header_offset_of_us, 0xFFF0);
  std::vector<BYTE> sparse_first_run = MakeSyntheticRecord0();
  // A header byte with no offset field makes the run sparse.
  Put<std::uint8_t>(sparse_first_run, first_attr_at + 0x40, 0x01);
  std::vector<BYTE> cluster_not_a_power = MakeSyntheticRecord0();
  Put<std::uint64_t>(cluster_not_a_power, first_attr_at + 0x28, 1536);
  std::vector<BYTE> magic_baad = MakeSyntheticRecord0();
  Put<std::uint32_t>(magic_baad, 0, 0x44414142);
  const std::vector<Case> cases = {
      {"an empty file", {}},
      {"fewer bytes than a header", std::vector<BYTE>(47)},
      {"a zero-filled file", std::vector<BYTE>(4096)},
      {"a BAAD record 0", magic_baad},
      {"a record size of 0", MakeEmptyRecord(1024, 0)},
      {"a record size of 256", MakeEmptyRecord(1024, 256)},
      {"a record size of 1000", MakeEmptyRecord(1024, 1000)},
      {"a record size of 1 MiB", MakeEmptyRecord(1024, 1U << 20U)},
      {"a record size past the file", MakeEmptyRecord(1024, 4096)},
      {"a record 0 without $DATA", MakeEmptyRecord(1024, 1024)},
      {"a failed update sequence", bad_sequence},
      {"an update sequence array too short", short_array},
      {"an update sequence array outside the record", array_out_of_record},
      {"a sparse first run", sparse_first_run},
      {"a cluster size that is not a power of two", cluster_not_a_power},
  };
  for (const Case& test : cases) {
    CAPTURE(test.label);
    const TempFile file(test.bytes);
    StandaloneMftDiskReader reader;
    CHECK_FALSE(reader.Open(file.Path().wstring()));
    CHECK_FALSE(reader.Failure().empty());
    CHECK_FALSE(reader.Size().has_value());
    LARGE_INTEGER addr{};
    std::vector<BYTE> read(16);
    CHECK_FALSE(reader.ReadInto(addr, read));
  }
}

TEST_CASE("A missing file is refused as a standalone $MFT",
          "[standalone-mft][synthetic]") {
  StandaloneMftDiskReader reader;
  CHECK_FALSE(reader.Open(
      (std::filesystem::temp_directory_path() / "no-such-mft-file").wstring()));
  CHECK_FALSE(reader.Failure().empty());
}

TEST_CASE("A $MFT cut after record 3 or inside a record still scans",
          "[dfir-corpora][integration][mft]") {
  const std::filesystem::path orphan = RequireCorpusFile(Dfir("orphan.mft"));
  const StandaloneMftScan full =
      ScanStandaloneMft(orphan, Cache::Strategy::NoCache, StrictOptions());

  SECTION("four records: the volume metadata of NTFS") {
    const TempFile file(Head(orphan, 4 * 1024));
    const StandaloneMftScan scan = ScanStandaloneMft(
        file.Path(), Cache::Strategy::NoCache, StrictOptions());
    // The cut file declares all 256 slots in record 0: the others fail.
    CHECK(scan.tree.Stats().slots == 256);
    CHECK(scan.tree.Stats().unreadable == 252);
    REQUIRE(scan.tree.Entries().size() == 4);
    CHECK(scan.tree.Entries().at(0).names.at(0).name == L"$MFT");
    CHECK(scan.tree.Entries().at(1).names.at(0).name == L"$MFTMirr");
    CHECK(scan.tree.Entries().at(2).names.at(0).name == L"$LogFile");
    CHECK(scan.tree.Entries().at(3).names.at(0).name == L"$Volume");
  }

  SECTION("a record cut at 300 of its 1024 bytes is unreadable") {
    // Record 43 is the last record the file keeps in part.
    const TempFile file(Head(orphan, 43 * 1024 + 300));
    const StandaloneMftScan scan = ScanStandaloneMft(
        file.Path(), Cache::Strategy::NoCache, StrictOptions());
    CHECK(scan.tree.Find(42) != nullptr);
    CHECK(scan.tree.Find(43) == nullptr);
    CHECK(scan.tree.Find(44) == nullptr);
    for (const Mft::MftTree::Entry& entry : scan.tree.Entries()) {
      CAPTURE(entry.record);
      const Mft::MftTree::Entry* whole = full.tree.Find(entry.record);
      REQUIRE(whole != nullptr);
      CHECK(FirstDifference(entry, *whole).empty());
    }
  }

  SECTION("one record is no volume: $Volume is missing") {
    const TempFile file(Head(orphan, 1024));
    auto reader = std::make_unique<StandaloneMftDiskReader>();
    REQUIRE(reader->Open(file.Path().wstring()));
    const NtfsVolume<Cache::Strategy::NoCache> volume(std::move(reader));
    CHECK_FALSE(volume.IsVolumeOK());
  }
}

TEST_CASE("The freed files of orphan.mft keep their lost parent",
          "[dfir-corpora][integration][mft][orphan]") {
  const std::filesystem::path file = RequireCorpusFile(Dfir("orphan.mft"));

  SECTION("without deleted items they are not listed") {
    const StandaloneMftScan scan =
        ScanStandaloneMft(file, Cache::Strategy::NoCache, StrictOptions(false));
    CHECK(scan.tree.Stats().deleted == 4);
    for (const ULONGLONG record : {44ULL, 45ULL, 46ULL, 47ULL}) {
      CHECK(scan.tree.Find(record) == nullptr);
    }
    // Record 39 is the live directory n1, reused at sequence 2.
    const Mft::MftTree::Entry* n1 = scan.tree.Find(39);
    REQUIRE(n1 != nullptr);
    CHECK(n1->in_use);
    CHECK(n1->directory);
    CHECK(n1->sequence == 2);
    CHECK(scan.tree.GetPath(39) == L"\\n1");
    CHECK(scan.tree.Children(39).empty());
  }

  SECTION("with deleted items they sit under no parent") {
    for (const Cache::Strategy strategy :
         {Cache::Strategy::NoCache, Cache::Strategy::FullCache}) {
      for (const VolumeOptions& options :
           {StrictOptions(), RecoveringOptions()}) {
        const StandaloneMftScan scan =
            ScanStandaloneMft(file, strategy, options);
        const Mft::MftTree& tree = scan.tree;
        const std::array<std::wstring_view, 4> names = {L"2.txt", L"3.txt",
                                                        L"4.txt", L"5.txt"};
        for (std::size_t index = 0; index < names.size(); index++) {
          const ULONGLONG record = 44 + index;
          CAPTURE(record);
          const Mft::MftTree::Entry* entry = tree.Find(record);
          REQUIRE(entry != nullptr);
          CHECK_FALSE(entry->in_use);
          CHECK(entry->sequence == 2);
          REQUIRE(entry->names.size() == 1);
          CHECK(entry->names.at(0).name == names.at(index));
          // The parent is record 39 as it was before its reuse.
          CHECK(entry->names.at(0).parent_record == 39);
          CHECK(entry->names.at(0).parent_sequence == 1);
          CHECK_FALSE(entry->names.at(0).parent_valid);
          CHECK_FALSE(tree.IsReachable(record));

          // The path is relative, to the record where the chain breaks.
          std::optional<ULONGLONG> lost;
          CHECK(tree.GetPath(record, &lost) == names.at(index));
          CHECK(lost == std::optional<ULONGLONG>(39));
        }
        // Not under the directory that reused the record number.
        CHECK(tree.Children(39).empty());
        CHECK(tree.IsReachable(39));
        CHECK(tree.Stats().deleted == 4);
      }
    }
  }
}

TEST_CASE("The freed subtree of deleted.mft stays linked",
          "[dfir-corpora][integration][mft][orphan]") {
  const std::filesystem::path file = RequireCorpusFile(Dfir("deleted.mft"));

  const StandaloneMftScan live_only =
      ScanStandaloneMft(file, Cache::Strategy::NoCache, StrictOptions(false));
  for (const ULONGLONG record : {39ULL, 43ULL, 44ULL, 45ULL, 46ULL, 47ULL}) {
    CHECK(live_only.tree.Find(record) == nullptr);
  }

  const StandaloneMftScan scan =
      ScanStandaloneMft(file, Cache::Strategy::NoCache, StrictOptions());

  // Six records freed at sequence 2, each parent named at sequence 1: NTFS
  // bumped the sequence of the parent when it freed it.
  struct Freed {
    ULONGLONG record;
    std::wstring_view path;
  };

  const std::array<Freed, 6> freed = {{
      {39, L"\\1"},
      {43, L"\\1\\2"},
      {44, L"\\1\\2\\3"},
      {45, L"\\1\\2\\33"},
      {46, L"\\1\\2\\3\\4"},
      {47, L"\\1\\2\\3\\4\\file.txt"},
  }};
  for (const Freed& file_record : freed) {
    CAPTURE(file_record.record);
    const Mft::MftTree::Entry* entry = scan.tree.Find(file_record.record);
    REQUIRE(entry != nullptr);
    CHECK_FALSE(entry->in_use);
    CHECK(entry->sequence == 2);
    REQUIRE(entry->names.size() == 1);
    CHECK(entry->names.at(0).parent_valid);
    CHECK(scan.tree.IsReachable(file_record.record));
    CHECK(scan.tree.GetPath(file_record.record) == file_record.path);
  }
  // Record 39's parent is the root, at sequence 5.
  CHECK(scan.tree.Find(39)->names.at(0).parent_record == 5);
  CHECK(scan.tree.Find(39)->names.at(0).parent_sequence == 5);
}

TEST_CASE("The Cyrillic names of unicode.mft come back",
          "[dfir-corpora][integration][mft]") {
  const std::filesystem::path file = RequireCorpusFile(Dfir("unicode.mft"));
  const StandaloneMftScan scan =
      ScanStandaloneMft(file, Cache::Strategy::NoCache, StrictOptions(false));

  // Привет, a directory, and привет.txt in it.
  const std::wstring directory = L"Привет";
  const std::wstring name = L"привет.txt";
  const Mft::MftTree::Entry* dir = scan.tree.Find(42);
  const Mft::MftTree::Entry* leaf = scan.tree.Find(43);
  REQUIRE(dir != nullptr);
  REQUIRE(leaf != nullptr);
  CHECK(dir->directory);
  CHECK_FALSE(leaf->directory);
  CHECK(dir->names.at(0).name == directory);
  CHECK(leaf->names.at(0).name == name);
  CHECK(leaf->names.at(0).parent_record == 42);
  CHECK(scan.tree.GetPath(43) == L"\\" + directory + L"\\" + name);
}

TEST_CASE("The volume label of orphan.mft is read from its $Volume record",
          "[dfir-corpora][integration][mft]") {
  const std::filesystem::path file = RequireCorpusFile(Dfir("orphan.mft"));
  const std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>> volume =
      OpenStandaloneMftVolume(file);
  CHECK(volume->GetVersion().first >= 3);

  FileRecord<Cache::Strategy::NoCache> record(*volume);
  record.SetAttrMask(Attr::Mask::VolumeName);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Volume)));
  REQUIRE(record.ParseAttrs());
  const auto& names = record.GetAttr(Attr::Type::VolumeName);
  REQUIRE(names.size() == 1);
  const auto& name = static_cast<const Attr::AttrVolName<
      NTFS_BROWSER_NAMESPACE::Io::ResidentOf<Cache::Strategy::NoCache>,
      Cache::Strategy::NoCache>&>(*names.front());
  std::wstring_view label = name.GetName();
  // The attribute is padded with a terminator its view covers.
  while (!label.empty() && label.back() == L'\0') {
    label.remove_suffix(1);
  }
  // "test" in Cyrillic letters, then in Latin letters.
  CHECK(label == L"\u0442\u0435\u0441\u0442-test");
}

TEST_CASE("Real 4096-byte records keep their resident data",
          "[dfir-corpora][integration][mft]") {
  const std::filesystem::path file = RequireCorpusFile(Dfir("4k-large.mft"));
  const std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>> volume =
      OpenStandaloneMftVolume(file);
  CHECK(volume->GetFileRecordSize() == 4096);
  CHECK(volume->GetRecordsCount() == 256);

  // Record 39 is 1.txt, with 3216 resident bytes, and record 43 is 2.txt, with
  // 44. Both MD5 come from Python's hashlib over the bytes of the $DATA.
  struct Resident {
    ULONGLONG record;
    ULONGLONG size;
    std::string_view md5;
  };

  const std::array<Resident, 2> files = {{
      {39, 3216, "a75a25c964f50df4ea9398d8ccf6afbd"},
      {43, 44, "d6fbed685c98416fb7388dad7503811c"},
  }};
  for (const Resident& expected : files) {
    CAPTURE(expected.record);
    FileRecord<Cache::Strategy::NoCache> record(*volume);
    record.SetAttrMask(Attr::Mask::Data);
    REQUIRE(record.ParseFileRecord(expected.record));
    REQUIRE(record.ParseAttrs());
    const auto& streams = record.GetAttr(Attr::Type::Data);
    REQUIRE(streams.size() == 1);
    const Attr::AttrBase<Cache::Strategy::NoCache>& data = *streams.front();
    REQUIRE(data.GetDataSize() == expected.size);
    CHECK(data.GetExtents().extents.empty());

    std::vector<BYTE> bytes(expected.size);
    REQUIRE(data.ReadData(0, bytes) == expected.size);
#ifdef NTFS_TEST_HAS_MD5
    CHECK(NTFS_BROWSER_NAMESPACE::Tests::Md5Hex(bytes) == expected.md5);
#endif
  }
}

TEST_CASE("The run lists of mapping_pairs_1.mft decode to their extents",
          "[dfir-corpora][integration][mft]") {
  const std::filesystem::path file =
      RequireCorpusFile(Dfir("mapping_pairs_1.mft"));
  const std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>> volume =
      OpenStandaloneMftVolume(file);

  using Extents = std::vector<NTFS_BROWSER_NAMESPACE::Alloc::Extent>;

  // A sparse run is a hole: it is not an extent. The last run of
  // fragmented.txt goes backward on the volume.
  struct Mapping {
    ULONGLONG record;
    Extents extents;
  };

  const std::vector<Mapping> mappings = {
      {41, {}},
      {44, {{1222, 4}, {1227, 9}, {1897, 150}, {1237, 41}}},
      {45, {{137, 16}}},
      {46, {{154, 1}}},
      {64, {{1216, 1}, {1226, 1}, {1236, 1}}},
      {73, {{1278, 1}, {1282, 1}}},
  };
  for (const Mapping& mapping : mappings) {
    CAPTURE(mapping.record);
    FileRecord<Cache::Strategy::NoCache> record(*volume);
    record.SetAttrMask(Attr::Mask::Data);
    REQUIRE(record.ParseFileRecord(mapping.record));
    REQUIRE(record.ParseAttrs());
    const auto& streams = record.GetAttr(Attr::Type::Data);
    REQUIRE_FALSE(streams.empty());
    const NTFS_BROWSER_NAMESPACE::Alloc::StreamExtents found =
        streams.front()->GetExtents();
    CHECK(found.complete);
    REQUIRE(found.extents.size() == mapping.extents.size());
    for (std::size_t index = 0; index < mapping.extents.size(); index++) {
      CHECK(found.extents.at(index).lcn == mapping.extents.at(index).lcn);
      CHECK(found.extents.at(index).clusters ==
            mapping.extents.at(index).clusters);
    }
  }
}

TEST_CASE("The fragmented $MFT of ntfs_frag_mft.bin reads in five runs",
          "[dfir-corpora][integration][mft]") {
  const std::filesystem::path image =
      RequireCorpusFile(Dfir("ntfs_frag_mft.bin"));
  // The volume starts at byte 65536 of the image.
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::PartitionDiskReader>(
          65536);
  REQUIRE(reader->Open(image.wstring()));
  const NtfsVolume<Cache::Strategy::NoCache> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetClusterSize() == 4096);
  CHECK(volume.GetFileRecordSize() == 1024);
  CHECK(volume.GetRecordsCount() == 3072);

  FileRecord<Cache::Strategy::NoCache> record(volume);
  record.SetAttrMask(Attr::Mask::Data);
  REQUIRE(record.ParseFileRecord(0));
  REQUIRE(record.ParseAttrs());
  const auto& streams = record.GetAttr(Attr::Type::Data);
  REQUIRE_FALSE(streams.empty());
  const NTFS_BROWSER_NAMESPACE::Alloc::StreamExtents found =
      streams.front()->GetExtents();
  using Extent = NTFS_BROWSER_NAMESPACE::Alloc::Extent;
  const std::array<Extent, 5> runs = {
      {{597, 235}, {1164, 116}, {1304, 136}, {1456, 112}, {1616, 169}}};
  REQUIRE(found.extents.size() == runs.size());
  for (std::size_t index = 0; index < runs.size(); index++) {
    CHECK(found.extents.at(index).lcn == runs.at(index).lcn);
    CHECK(found.extents.at(index).clusters == runs.at(index).clusters);
  }

#ifdef NTFS_TEST_HAS_MD5
  // The MD5 of the five runs, one after the other, taken from the image by
  // Python.
  std::vector<BYTE> mft(3145728);
  REQUIRE(volume.ReadMftData(0, mft) == mft.size());
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Md5Hex(mft) ==
        "3a75da5a96eeab4850b811df1c6b6ec9");
#endif
}
