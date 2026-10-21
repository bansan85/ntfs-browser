#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <random>
#include <string>
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

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "record/slack-carver.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Record = NTFS_BROWSER_NAMESPACE::Record;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeIndexBlock;
using Tests::FakeSlackName;

namespace {

// Index block size of the fixtures: one cluster of the fake volume.
constexpr DWORD block_size = Tests::fake_cluster_size;

// Where the entries of a block stop and its fixed layout starts, see
// BuildFakeIndexBlock(): the end entry of an empty block sits at 0x40.
constexpr size_t empty_used_end = 0x50;

const VolumeOptions both_flags{.include_deleted = true, .recover_errors = true};

// Calls check once per cache strategy.
template <typename Check>
void ForEachStrategy(Check check) {
  check(std::integral_constant<Cache::Strategy, Cache::Strategy::NoCache>{});
  check(std::integral_constant<Cache::Strategy, Cache::Strategy::FullCache>{});
}

// A name as the used part of a block holds it.
FakeSlackName Live(std::wstring name) {
  return FakeSlackName{.name = std::move(name),
                       .parent_record = Tests::index_slack_directory_record,
                       .parent_sequence =
                           Tests::index_slack_directory_sequence};
}

// A stale name of the directory, with its index entry header.
FakeSlackName Old(std::wstring name) {
  FakeSlackName stale = Live(std::move(name));
  stale.with_entry_header = true;
  return stale;
}

// The bytes of several stale names, one after the other.
std::vector<BYTE> Stale(const std::vector<FakeSlackName>& names) {
  std::vector<BYTE> bytes;
  for (const FakeSlackName& name : names) {
    const std::vector<BYTE> one = Tests::EncodeSlackName(name);
    bytes.insert(bytes.end(), one.begin(), one.end());
  }
  return bytes;
}

// A block holding live and stale names, VCN vcn.
std::vector<BYTE> Block(const std::vector<std::wstring>& live,
                        const std::vector<FakeSlackName>& stale,
                        ULONGLONG vcn = 0) {
  FakeIndexBlock block{.slack = Stale(stale), .vcn = vcn};
  for (const std::wstring& name : live) {
    block.live.push_back(Live(name));
  }
  return Tests::BuildFakeIndexBlock(block, block_size);
}

// What a directory record gives for its index slack, for one image.
template <Cache::Strategy S>
std::vector<Record::CarvedName> Carve(std::vector<BYTE> image,
                                      const VolumeOptions& options) {
  NtfsVolume<S> volume(
      std::make_unique<Tests::MemoryDiskReader>(std::move(image)), options);
  REQUIRE(volume.IsVolumeOK());
  Io::FileRecord<S> directory(volume);
  REQUIRE(directory.ParseFileRecord(Tests::index_slack_directory_record));
  REQUIRE(directory.ParseAttrs());
  return directory.CarveIndexSlackNames();
}

std::vector<Record::CarvedName>
    CarveBlocks(const std::vector<std::vector<BYTE>>& blocks,
                const VolumeOptions& options = both_flags) {
  return Carve<Cache::Strategy::NoCache>(
      Tests::BuildFakeNtfsImageWithIndexBlocks(blocks), options);
}

std::vector<std::wstring> Names(const std::vector<Record::CarvedName>& carved) {
  std::vector<std::wstring> names;
  for (const Record::CarvedName& name : carved) {
    names.push_back(name.name);
  }
  return names;
}

}  // namespace

TEST_CASE(
    "Index slack gives the older entries of a block, apart from live ones",
    "[index-slack]") {
  ForEachStrategy([](auto strategy) {
    const std::vector<std::vector<BYTE>> blocks{
        Block({L"live-a.txt", L"live-b.txt"},
              {Old(L"older.txt"), Old(L"oldest.txt")})};

    const auto carved = Carve<decltype(strategy)::value>(
        Tests::BuildFakeNtfsImageWithIndexBlocks(blocks), both_flags);

    REQUIRE(carved.size() == 2);
    CHECK(carved.at(0).name == L"older.txt");
    CHECK(carved.at(1).name == L"oldest.txt");
    for (const Record::CarvedName& name : carved) {
      CHECK(name.parent_known);
      CHECK(name.parent_record == Tests::index_slack_directory_record);
      CHECK(name.parent_sequence == Tests::index_slack_directory_sequence);
      CHECK(name.times_known);
      CHECK_FALSE(name.partial);
      CHECK(name.source == Record::CarveSource::IndexSlack);
    }
  });
}

TEST_CASE("Index slack of a structure without its entry header",
          "[index-slack]") {
  // Where the end entry overwrote the header, the name starts the slack.
  FakeSlackName bare = Live(L"bare.txt");
  const std::vector<std::vector<BYTE>> blocks{Tests::BuildFakeIndexBlock(
      FakeIndexBlock{.live = {Live(L"live.txt")},
                     .slack = Tests::EncodeSlackName(bare)},
      block_size)};

  CHECK(Names(CarveBlocks(blocks)) == std::vector<std::wstring>{L"bare.txt"});
}

TEST_CASE("Index slack leaves out a name that a live entry repeats",
          "[index-slack]") {
  const std::vector<std::vector<BYTE>> blocks{
      Block({L"same.txt", L"other.txt"}, {Old(L"same.txt"), Old(L"gone.txt")})};

  CHECK(Names(CarveBlocks(blocks)) == std::vector<std::wstring>{L"gone.txt"});
}

TEST_CASE(
    "Index slack leaves out a name that a live entry of another block repeats",
    "[index-slack]") {
  const std::vector<std::vector<BYTE>> blocks{
      Block({L"first.txt"}, {Old(L"second.txt")}, 0),
      Block({L"second.txt"}, {Old(L"first.txt"), Old(L"third.txt")}, 1)};

  CHECK(Names(CarveBlocks(blocks)) == std::vector<std::wstring>{L"third.txt"});
}

TEST_CASE("Index slack reports a name once, whatever the number of copies",
          "[index-slack]") {
  const std::vector<std::vector<BYTE>> blocks{
      Block({L"a.txt"}, {Old(L"copy.txt"), Old(L"copy.txt")}, 0),
      Block({L"b.txt"}, {Old(L"copy.txt")}, 1)};

  CHECK(Names(CarveBlocks(blocks)) == std::vector<std::wstring>{L"copy.txt"});
}

TEST_CASE("Index slack keeps two names that differ by their times",
          "[index-slack]") {
  FakeSlackName later = Old(L"same.txt");
  later.times = {0x01D68C7C1B2B0000ULL, 0x01D68C7C1B2B0100ULL,
                 0x01D68C7C1B2B0200ULL, 0x01D68C7C1B2B0300ULL};
  const std::vector<std::vector<BYTE>> blocks{
      Block({L"a.txt"}, {Old(L"same.txt"), later})};

  CHECK(CarveBlocks(blocks).size() == 2);
}

TEST_CASE("Index slack drops a name filed under another directory",
          "[index-slack]") {
  FakeSlackName elsewhere = Old(L"elsewhere.txt");
  elsewhere.parent_record = 999;
  FakeSlackName older_generation = Old(L"generation.txt");
  older_generation.parent_sequence = Tests::index_slack_directory_sequence + 1;
  FakeSlackName unchecked = Old(L"unchecked.txt");
  unchecked.parent_sequence = 0;
  const std::vector<std::vector<BYTE>> blocks{Block(
      {L"a.txt"}, {elsewhere, older_generation, unchecked, Old(L"mine.txt")})};

  CHECK(Names(CarveBlocks(blocks)) ==
        std::vector<std::wstring>{L"unchecked.txt", L"mine.txt"});
}

TEST_CASE("Index slack drops a name whose parent reference was overwritten",
          "[index-slack]") {
  std::vector<BYTE> lost = Tests::EncodeSlackName(Old(L"lost-parent.txt"));
  // The parent reference is the first 8 bytes after the entry header.
  std::fill_n(lost.begin() + 16, 8, BYTE{0});
  std::vector<BYTE> slack = lost;
  const std::vector<BYTE> kept = Tests::EncodeSlackName(Old(L"kept.txt"));
  slack.insert(slack.end(), kept.begin(), kept.end());
  const std::vector<std::vector<BYTE>> blocks{Tests::BuildFakeIndexBlock(
      FakeIndexBlock{.live = {Live(L"a.txt")}, .slack = slack}, block_size)};

  CHECK(Names(CarveBlocks(blocks)) == std::vector<std::wstring>{L"kept.txt"});
}

TEST_CASE("Index slack skips a block that fails its checks and goes on",
          "[index-slack]") {
  std::vector<BYTE> bad_magic =
      Block({L"a.txt"}, {Old(L"in-bad-magic.txt")}, 0);
  bad_magic.at(0) = 'X';
  std::vector<BYTE> bad_usn = Block({L"b.txt"}, {Old(L"in-bad-usn.txt")}, 1);
  // The check word of the first sector no longer matches the array.
  bad_usn.at(510) ^= 0xFF;
  std::vector<BYTE> bad_us_offset =
      Block({L"c.txt"}, {Old(L"in-bad-offset.txt")}, 2);
  bad_us_offset.at(4) = 0xFF;
  bad_us_offset.at(5) = 0xFF;
  const std::vector<std::vector<BYTE>> blocks{
      bad_magic, bad_usn, bad_us_offset,
      Block({L"d.txt"}, {Old(L"survivor.txt")}, 3)};

  CHECK(Names(CarveBlocks(blocks)) ==
        std::vector<std::wstring>{L"survivor.txt"});
}

TEST_CASE("Index slack skips a block whose entries are damaged",
          "[index-slack]") {
  std::vector<BYTE> zero_size =
      Block({L"a.txt", L"b.txt"}, {Old(L"hidden.txt")}, 0);
  // The first entry claims a size of 0: the walk cannot move on.
  zero_size.at(0x40 + 8) = 0;
  zero_size.at(0x40 + 9) = 0;
  std::vector<BYTE> past_end = Block({L"c.txt"}, {Old(L"hidden-too.txt")}, 1);
  // The first entry claims to run past the end of the block.
  past_end.at(0x40 + 8) = 0xF0;
  past_end.at(0x40 + 9) = 0xFF;
  const std::vector<std::vector<BYTE>> blocks{
      zero_size, past_end, Block({L"d.txt"}, {Old(L"fine.txt")}, 2)};

  CHECK(Names(CarveBlocks(blocks)) == std::vector<std::wstring>{L"fine.txt"});
}

TEST_CASE(
    "Index slack reads a name that crosses a sector boundary after the fixups",
    "[index-slack]") {
  // The end entry of an empty block ends at 0x50. Pad so that the structure
  // starts at byte 500: bytes 510 and 511, the check word, then sit inside its
  // creation time.
  FakeIndexBlock block;
  const std::vector<BYTE> name =
      Tests::EncodeSlackName(Old(L"straddles-the-sector.txt"));
  block.slack.assign(500 - 16 - empty_used_end, 0);
  block.slack.insert(block.slack.end(), name.begin(), name.end());
  const std::vector<std::vector<BYTE>> blocks{
      Tests::BuildFakeIndexBlock(block, block_size)};

  CHECK(Names(CarveBlocks(blocks)) ==
        std::vector<std::wstring>{L"straddles-the-sector.txt"});
}

TEST_CASE("Index slack of a block with no room for any candidate",
          "[index-slack]") {
  const FakeIndexBlock none{.live = {Live(L"a.txt")}};
  const FakeIndexBlock four{.live = {Live(L"a.txt")}, .slack = {1, 2, 3, 4}};
  // The slack runs to the last byte of the block.
  FakeIndexBlock full{.live = {Live(L"a.txt")}};
  const size_t used = 0x40 + Tests::EncodeSlackName(Old(L"a.txt")).size() + 16;
  full.slack.assign(block_size - used, 0x20);
  const std::vector<std::vector<BYTE>> blocks{
      Tests::BuildFakeIndexBlock(none, block_size),
      Tests::BuildFakeIndexBlock(four, block_size),
      Tests::BuildFakeIndexBlock(full, block_size)};

  CHECK(CarveBlocks(blocks).empty());
}

TEST_CASE("Index slack ignores a name cut by the end of the block",
          "[index-slack]") {
  const std::vector<BYTE> whole =
      Tests::EncodeSlackName(Old(L"cut-by-the-end.txt"));
  FakeIndexBlock block{.live = {Live(L"a.txt")}};
  block.slack = Stale({Old(L"complete.txt")});
  const size_t used = 0x40 + Tests::EncodeSlackName(Old(L"a.txt")).size() + 16;
  // The entry header, the $FILE_NAME up to its name and 5 of the characters
  // survive, then the block ends.
  constexpr size_t kept = 16 + 0x42 + 10;
  block.slack.insert(block.slack.end(),
                     block_size - used - block.slack.size() - kept, 0);
  block.slack.insert(block.slack.end(), whole.begin(), whole.begin() + kept);
  const std::vector<std::vector<BYTE>> blocks{
      Tests::BuildFakeIndexBlock(block, block_size)};

  CHECK(Names(CarveBlocks(blocks)) ==
        std::vector<std::wstring>{L"complete.txt"});
}

TEST_CASE("Index slack ends on any content", "[index-slack]") {
  std::mt19937 random(7);
  std::vector<std::vector<BYTE>> blocks;
  // Zeros, ones, a repeated entry header of size zero, a repeated tiny
  // header, and random bytes.
  const std::vector<BYTE> fills[] = {
      std::vector<BYTE>(500, 0x00), std::vector<BYTE>(500, 0xFF),
      std::vector<BYTE>(500, 0x01), std::vector<BYTE>(500, 0x20)};
  ULONGLONG vcn = 0;
  for (const std::vector<BYTE>& fill : fills) {
    blocks.push_back(Tests::BuildFakeIndexBlock(
        FakeIndexBlock{.live = {Live(L"a.txt")}, .slack = fill, .vcn = vcn++},
        block_size));
  }
  std::vector<BYTE> noise(500);
  for (int round = 0; round < 8; ++round) {
    std::ranges::generate(noise, [&] { return static_cast<BYTE>(random()); });
    blocks.push_back(Tests::BuildFakeIndexBlock(
        FakeIndexBlock{.live = {Live(L"a.txt")}, .slack = noise, .vcn = vcn++},
        block_size));
  }

  CHECK(CarveBlocks(blocks).empty());
}

TEST_CASE("Index slack stays empty without recover_errors and include_deleted",
          "[index-slack]") {
  ForEachStrategy([](auto strategy) {
    const std::vector<std::vector<BYTE>> blocks{
        Block({L"a.txt"}, {Old(L"older.txt")})};
    const std::vector<BYTE> image =
        Tests::BuildFakeNtfsImageWithIndexBlocks(blocks);

    CHECK(Carve<decltype(strategy)::value>(image, VolumeOptions{}).empty());
    CHECK(Carve<decltype(strategy)::value>(
              image, VolumeOptions{.include_deleted = true})
              .empty());
    CHECK(Carve<decltype(strategy)::value>(
              image, VolumeOptions{.recover_errors = true})
              .empty());
    CHECK(Carve<decltype(strategy)::value>(image, both_flags).size() == 1);
  });
}

TEST_CASE("Index slack reads nothing from the volume when it is off",
          "[index-slack]") {
  const std::vector<std::vector<BYTE>> blocks{
      Block({L"a.txt"}, {Old(L"older.txt")})};
  for (const VolumeOptions& options :
       {VolumeOptions{}, VolumeOptions{.include_deleted = true},
        VolumeOptions{.recover_errors = true}}) {
    auto counter = std::make_unique<Tests::CountingDiskReader>(
        std::make_unique<Tests::MemoryDiskReader>(
            Tests::BuildFakeNtfsImageWithIndexBlocks(blocks)));
    Tests::CountingDiskReader* reads = counter.get();
    NtfsVolume<Cache::Strategy::NoCache> volume(std::move(counter), options);
    REQUIRE(volume.IsVolumeOK());
    Io::FileRecord<Cache::Strategy::NoCache> directory(volume);
    REQUIRE(directory.ParseFileRecord(Tests::index_slack_directory_record));
    REQUIRE(directory.ParseAttrs());
    reads->Reset();

    CHECK(directory.CarveIndexSlackNames().empty());

    CHECK(reads->ReadCount() == 0);
  }
}

TEST_CASE("Index slack changes no live listing", "[index-slack]") {
  const std::vector<std::vector<BYTE>> blocks{
      Block({L"a.txt"}, {Old(L"older.txt")})};
  const auto listing = [&](bool carve) {
    NtfsVolume<Cache::Strategy::NoCache> volume(
        std::make_unique<Tests::MemoryDiskReader>(
            Tests::BuildFakeNtfsImageWithIndexBlocks(blocks)),
        both_flags);
    REQUIRE(volume.IsVolumeOK());
    Io::FileRecord<Cache::Strategy::NoCache> directory(volume);
    REQUIRE(directory.ParseFileRecord(Tests::index_slack_directory_record));
    REQUIRE(directory.ParseAttrs());
    if (carve) {
      CHECK(directory.CarveIndexSlackNames().size() == 1);
    }
    std::vector<std::wstring> seen;
    directory.TraverseSubEntries(
        [&seen](const NTFS_BROWSER_NAMESPACE::IndexEntryView& entry, void*) {
          seen.emplace_back(entry.GetFilename());
        },
        nullptr);
    return seen;
  };

  CHECK(listing(true) == listing(false));
  CHECK(listing(false) == std::vector<std::wstring>{L"a.txt"});
}

TEST_CASE("Index slack of a directory without index allocation",
          "[index-slack]") {
  ForEachStrategy([](auto strategy) {
    CHECK(Carve<decltype(strategy)::value>(
              Tests::BuildFakeNtfsImageWithRootIndexRootEntry(), both_flags)
              .empty());
  });
}

TEST_CASE("Index slack of both cache strategies agrees", "[index-slack]") {
  const std::vector<std::vector<BYTE>> blocks{
      Block({L"a.txt"}, {Old(L"one.txt"), Old(L"two.txt")}, 0),
      Block({L"b.txt"}, {Old(L"three.txt")}, 1)};
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexBlocks(blocks);

  const auto no_cache = Carve<Cache::Strategy::NoCache>(image, both_flags);
  const auto full_cache = Carve<Cache::Strategy::FullCache>(image, both_flags);

  REQUIRE(no_cache.size() == 3);
  REQUIRE(full_cache.size() == 3);
  for (size_t i = 0; i < no_cache.size(); ++i) {
    CHECK(Record::SameCarvedName(no_cache.at(i), full_cache.at(i)));
  }
}

TEST_CASE("CarveFileNamesFrom starts at the slack and no earlier",
          "[index-slack]") {
  const std::vector<BYTE> name = Tests::EncodeSlackName(Live(L"inside.txt"));
  std::vector<BYTE> bytes(64, 0);
  bytes.insert(bytes.end(), name.begin(), name.end());
  bytes.insert(bytes.end(), 64, 0);

  const auto from_start =
      Record::CarveFileNamesFrom(bytes, 64, Record::CarveSource::IndexSlack);
  // A structure whose first fields lie before the slack is partial.
  const auto from_inside = Record::CarveFileNamesFrom(
      bytes, 64 + 8, Record::CarveSource::IndexSlack);

  REQUIRE(from_start.size() == 1);
  CHECK(from_start.at(0).name == L"inside.txt");
  CHECK_FALSE(from_start.at(0).partial);
  CHECK(from_start.at(0).source == Record::CarveSource::IndexSlack);
  REQUIRE(from_inside.size() == 1);
  CHECK(from_inside.at(0).partial);
  CHECK_FALSE(from_inside.at(0).parent_known);
}

TEST_CASE("CarveFileNamesFrom accepts a begin at or past the end",
          "[index-slack]") {
  const std::vector<BYTE> bytes(128, 0x41);

  CHECK(Record::CarveFileNamesFrom(bytes, 128, Record::CarveSource::IndexSlack)
            .empty());
  CHECK(Record::CarveFileNamesFrom(bytes, 129, Record::CarveSource::IndexSlack)
            .empty());
  CHECK(Record::CarveFileNamesFrom(bytes, ~size_t{0},
                                   Record::CarveSource::IndexSlack)
            .empty());
  CHECK(Record::CarveFileNamesFrom({}, 0, Record::CarveSource::IndexSlack)
            .empty());
}
