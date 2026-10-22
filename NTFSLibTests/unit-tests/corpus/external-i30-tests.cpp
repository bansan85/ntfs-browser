#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/record/carved-name.h>
#include <ntfs-browser/volume-options.h>

#include "corpus-test-support.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfs-browser/disk-reader.h"
#include "partition-disk-reader.h"

namespace {

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Record = NTFS_BROWSER_NAMESPACE::Record;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

constexpr VolumeOptions both_flags{.include_deleted = true,
                                   .recover_errors = true};

// The INDX blocks of Zimmerman's $I30 states are 4096 bytes long, and a file
// holds five of them.
constexpr size_t index_block_bytes = 4096;
constexpr size_t index_block_count = 5;

// Bytes per sector: the fixups sit at the end of each.
constexpr size_t sector_bytes = 512;

// The directory the three states belong to: record 118852 at sequence 18.
constexpr ULONGLONG zimmerman_parent = 118852;
constexpr ULONGLONG zimmerman_sequence = 18;

// Namespace byte of a DOS-only alias.
constexpr BYTE dos_name_space = 2;

std::filesystem::path StateFile(std::string_view state) {
  const std::u8string name(state.begin(), state.end());
  return Tests::RequireCorpusFile(Tests::DfirMuseumDir() / u8"Windows" /
                                  u8"NTFSArtifacts" / u8"$I30" /
                                  u8"EricZimmerman" / name / u8"$I30");
}

std::uint64_t Load64(const std::vector<BYTE>& bytes, size_t offset) {
  std::uint64_t value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

void Store64(std::vector<BYTE>& bytes, size_t offset, std::uint64_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// Puts the parent reference of every live and stale name in a block back to
// the directory the fake volume has. The fixture files name their directory
// 118852/18, a record the fake volume cannot hold, and the library only
// reports a name filed under the directory it lists. This changes the
// fixture: nothing else in the block moves. The block is on disk form, with
// its fixups, and leaves in the same form.
void RetargetParent(std::vector<BYTE>& block, std::uint64_t from,
                    std::uint64_t to) {
  uint16_t usa_offset = 0;
  std::memcpy(&usa_offset, block.data() + 4, sizeof(usa_offset));
  std::vector<BYTE> restored = block;
  const size_t sectors = block.size() / sector_bytes;
  for (size_t sector = 0; sector < sectors; ++sector) {
    std::memcpy(restored.data() + ((sector + 1) * sector_bytes) - 2,
                block.data() + usa_offset + 2 + (sector * 2), 2);
  }
  for (size_t at = 0; at + sizeof(from) <= restored.size(); ++at) {
    if (Load64(restored, at) == from) {
      Store64(restored, at, to);
    }
  }
  for (size_t sector = 0; sector < sectors; ++sector) {
    std::memcpy(block.data() + ((sector + 1) * sector_bytes) - 2,
                block.data() + usa_offset, 2);
    std::memcpy(block.data() + usa_offset + 2 + (sector * 2),
                restored.data() + ((sector + 1) * sector_bytes) - 2, 2);
  }
  for (size_t at = 0; at < restored.size(); ++at) {
    const bool is_check_word = (at % sector_bytes) >= sector_bytes - 2;
    if (!is_check_word) {
      block[at] = restored[at];
    }
  }
}

// A fake volume whose directory 5 holds the five blocks of a state.
std::vector<BYTE> ImageOfState(std::string_view state) {
  const std::vector<BYTE> file = Tests::ReadCorpusFile(StateFile(state));
  REQUIRE(file.size() == index_block_bytes * index_block_count);
  std::vector<std::vector<BYTE>> blocks;
  for (size_t index = 0; index < index_block_count; ++index) {
    const auto first =
        file.begin() + static_cast<std::ptrdiff_t>(index * index_block_bytes);
    std::vector<BYTE> block(first, first + index_block_bytes);
    RetargetParent(
        block, zimmerman_parent | (zimmerman_sequence << 48U),
        Tests::index_slack_directory_record |
            (std::uint64_t{Tests::index_slack_directory_sequence} << 48U));
    blocks.push_back(std::move(block));
  }
  return Tests::BuildFakeNtfsImageWithIndexBlocks(blocks);
}

// The names a state gives: the live listing and the slack candidates, DOS
// aliases left out as the reference does.
struct Listing {
  std::set<std::wstring> live;
  std::set<std::wstring> deleted;
  size_t live_entries{0};
  size_t candidates{0};
};

Listing ListState(std::string_view state) {
  NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<Tests::MemoryDiskReader>(ImageOfState(state)),
      both_flags);
  REQUIRE(volume.IsVolumeOK());
  Io::FileRecord<Cache::Strategy::NoCache> directory(volume);
  REQUIRE(directory.ParseFileRecord(Tests::index_slack_directory_record));
  REQUIRE(directory.ParseAttrs());

  Listing listing;
  directory.TraverseSubEntries(
      [&listing](const NTFS_BROWSER_NAMESPACE::IndexEntryView& entry, void*) {
        listing.live_entries++;
        listing.live.emplace(entry.GetFilename());
      },
      nullptr);
  for (const Record::CarvedName& name : directory.CarveIndexSlackNames()) {
    listing.candidates++;
    if (name.name_space != dos_name_space) {
      listing.deleted.insert(name.name);
    }
  }
  return listing;
}

// New Text Document - Copy (from).txt to (to).txt.
std::set<std::wstring> Copies(int from, int to) {
  std::set<std::wstring> names;
  for (int number = from; number <= to; ++number) {
    names.insert(L"New Text Document - Copy (" + std::to_wstring(number) +
                 L").txt");
  }
  return names;
}

// SomeFile<from>.txt to SomeFile<to>.txt.
std::set<std::wstring> SomeFiles(int from, int to) {
  std::set<std::wstring> names;
  for (int number = from; number <= to; ++number) {
    names.insert(L"SomeFile" + std::to_wstring(number) + L".txt");
  }
  return names;
}

// The deleted names the reference documents for each state: what the first
// delete adds to Start, then what the second adds to that.
std::set<std::wstring> FirstDeleteNames() {
  std::set<std::wstring> names = Copies(10, 19);
  const std::set<std::wstring> some_files = SomeFiles(2, 9);
  names.insert(some_files.begin(), some_files.end());
  names.insert({L"Folder5", L"Folder5a", L"SomeFile12.txt"});
  return names;
}

std::set<std::wstring> SecondDeleteNames() {
  std::set<std::wstring> names = FirstDeleteNames();
  names.insert({L"SomeFile17.txt", L"SomeFile18.txt", L"SomeFile20.txt"});
  return names;
}

}  // namespace

TEST_CASE("The index slack of Zimmerman's $I30 states names the deleted files",
          "[dfir-corpora][integration][i30]") {
  const Listing start = ListState("Start");
  const Listing first = ListState("FirstDelete");
  const Listing second = ListState("SecondDelete");

  // Live entries first. Zimmerman's own totals (88, 62, 57) are not asserted:
  // they cannot be rebuilt from his description.
  CHECK(start.live_entries == 50);
  CHECK(first.live_entries == 20);
  CHECK(second.live_entries == 10);

  CHECK((start.deleted == Copies(10, 19)));
  CHECK((first.deleted == FirstDeleteNames()));
  CHECK((second.deleted == SecondDeleteNames()));
  CHECK(start.deleted.size() == 10);
  CHECK(first.deleted.size() == 21);
  CHECK(second.deleted.size() == 24);

  // The deleted names only grow as files are deleted.
  CHECK(std::ranges::includes(first.deleted, start.deleted));
  CHECK(std::ranges::includes(second.deleted, first.deleted));
  // None of them is a live name of its own state.
  for (const Listing* state : {&start, &first, &second}) {
    for (const std::wstring& name : state->deleted) {
      CHECK_FALSE(state->live.contains(name));
    }
  }
}

TEST_CASE("Both cache strategies give the index slack of a $I30 state",
          "[dfir-corpora][integration][i30]") {
  const std::vector<BYTE> image = ImageOfState("SecondDelete");
  const auto carve = [&image](auto strategy) {
    NtfsVolume<decltype(strategy)::value> volume(
        std::make_unique<Tests::MemoryDiskReader>(image), both_flags);
    REQUIRE(volume.IsVolumeOK());
    Io::FileRecord<decltype(strategy)::value> directory(volume);
    REQUIRE(directory.ParseFileRecord(Tests::index_slack_directory_record));
    REQUIRE(directory.ParseAttrs());
    return directory.CarveIndexSlackNames();
  };

  const auto no_cache = carve(
      std::integral_constant<Cache::Strategy, Cache::Strategy::NoCache>{});
  const auto full_cache = carve(
      std::integral_constant<Cache::Strategy, Cache::Strategy::FullCache>{});

  REQUIRE(no_cache.size() == full_cache.size());
  CHECK_FALSE(no_cache.empty());
  for (size_t index = 0; index < no_cache.size(); ++index) {
    CHECK(no_cache.at(index).name == full_cache.at(index).name);
  }
}

TEST_CASE("Strict mode lists the live entries of a $I30 state only",
          "[dfir-corpora][integration][i30]") {
  NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<Tests::MemoryDiskReader>(ImageOfState("Start")));
  REQUIRE(volume.IsVolumeOK());
  Io::FileRecord<Cache::Strategy::NoCache> directory(volume);
  REQUIRE(directory.ParseFileRecord(Tests::index_slack_directory_record));
  REQUIRE(directory.ParseAttrs());

  CHECK(directory.CarveIndexSlackNames().empty());
}

TEST_CASE("The index slack of ntfs_index.raw names its one deleted file",
          "[dfir-corpora][integration][i30]") {
  const std::filesystem::path image = Tests::RequireCorpusFile(
      Tests::DfirNtfsDir() / u8"test_data" / u8"ntfs_index.raw");
  // The volume starts at byte 65536 of the image.
  auto reader = std::make_unique<Tests::PartitionDiskReader>(65536);
  REQUIRE(reader->Open(image.wstring()));
  const NtfsVolume<Cache::Strategy::NoCache> volume(std::move(reader),
                                                    both_flags);
  REQUIRE(volume.IsVolumeOK());

  // Record 39 is test_dir: its block repeats a live name and keeps one
  // deleted file.
  constexpr ULONGLONG test_dir_record = 39;
  Io::FileRecord<Cache::Strategy::NoCache> test_dir(volume);
  REQUIRE(test_dir.ParseFileRecord(test_dir_record));
  REQUIRE(test_dir.ParseAttrs());
  const auto carved = test_dir.CarveIndexSlackNames();
  REQUIRE(carved.size() == 1);
  CHECK(carved.at(0).name == L"BBBBBBBBBBBBB-del.txt");
  CHECK(carved.at(0).parent_known);
  CHECK(carved.at(0).parent_record == test_dir_record);

  // The root's blocks hold seven stale names, among them test_dir. Each one
  // repeats a live entry of another block, so none is a candidate.
  constexpr ULONGLONG root_record = 5;
  Io::FileRecord<Cache::Strategy::NoCache> root(volume);
  REQUIRE(root.ParseFileRecord(root_record));
  REQUIRE(root.ParseAttrs());
  CHECK(root.CarveIndexSlackNames().empty());
}
