#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "fuzz-core.h"
#include "memory-disk-reader.h"
#include "ntfs-browser/disk-reader.h"
#include "recording-disk-reader.h"

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeSlackName;

namespace {

// One saved fuzz input: its file name, and the image it was recorded from.
struct Variant {
  std::string name;
  std::vector<BYTE> image;
};

// Index block size of the fake volume: one cluster.
constexpr DWORD block_size = Tests::fake_cluster_size;

// Bytes a block of one live entry keeps after its end entry.
constexpr size_t slack_room = 800;

FakeSlackName Old(std::wstring name) {
  return FakeSlackName{.name = std::move(name),
                       .parent_record = Tests::index_slack_directory_record,
                       .parent_sequence = Tests::index_slack_directory_sequence,
                       .with_entry_header = true};
}

// A block with one live entry and the given slack.
std::vector<BYTE> BlockWith(std::vector<BYTE> slack, ULONGLONG vcn = 0) {
  slack.resize(std::min(slack.size(), slack_room));
  return Tests::BuildFakeIndexBlock(
      Tests::FakeIndexBlock{
          .live = {Old(L"live.txt")}, .slack = std::move(slack), .vcn = vcn},
      block_size);
}

// Candidates on top of each other: the same structure written at every even
// offset, each copy cutting the one before it.
std::vector<BYTE> DenseNames() {
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(Old(L"dense.txt"));
  std::vector<BYTE> slack(slack_room);
  for (size_t offset = 0; offset + bytes.size() <= slack.size(); offset += 2) {
    std::ranges::copy(bytes,
                      slack.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return slack;
}

// A name of 255 characters whose end the last byte of the slack cuts off,
// after a name that fits.
std::vector<BYTE> NameRunsPastTheBlock() {
  const std::vector<BYTE> bytes =
      Tests::EncodeSlackName(Old(std::wstring(255, L'n')));
  std::vector<BYTE> slack = Tests::EncodeSlackName(Old(L"keeper.txt"));
  slack.resize(slack_room);
  const size_t start = slack.size() - 200;
  std::ranges::copy(std::span(bytes).first(200),
                    slack.begin() + static_cast<std::ptrdiff_t>(start));
  return slack;
}

// Blocks that fail one check each, around a block that is sound.
std::vector<BYTE> DamagedBlocksImage() {
  std::vector<BYTE> bad_magic =
      BlockWith(Tests::EncodeSlackName(Old(L"a.txt")), 0);
  bad_magic.at(0) = 'X';
  std::vector<BYTE> zero_entry =
      BlockWith(Tests::EncodeSlackName(Old(L"b.txt")), 1);
  // The first entry claims a size of 0.
  zero_entry.at(0x40 + 8) = 0;
  zero_entry.at(0x40 + 9) = 0;
  std::vector<BYTE> bad_usn =
      BlockWith(Tests::EncodeSlackName(Old(L"c.txt")), 2);
  bad_usn.at(510) ^= 0xFF;
  const std::vector<std::vector<BYTE>> blocks{
      bad_magic, zero_entry, bad_usn,
      BlockWith(Tests::EncodeSlackName(Old(L"survivor.txt")), 3)};
  return Tests::BuildFakeNtfsImageWithIndexBlocks(blocks);
}

std::vector<BYTE> ImageOfBlock(std::vector<BYTE> slack) {
  const std::vector<std::vector<BYTE>> blocks{BlockWith(std::move(slack))};
  return Tests::BuildFakeNtfsImageWithIndexBlocks(blocks);
}

// The inputs, one per way a hostile slack can be wrong.
std::vector<Variant> Variants() {
  return {{"index_slack_dense_names", ImageOfBlock(DenseNames())},
          {"index_slack_name_runs_past_block",
           ImageOfBlock(NameRunsPastTheBlock())},
          {"index_slack_damaged_blocks", DamagedBlocksImage()}};
}

// Bytes of the boot sector read a NoCache volume issues first, and how far the
// fuzzer's reader moves the stream after it.
constexpr size_t boot_sector_read_bytes = 4096;
constexpr size_t boot_sector_stream_bytes = 512;

}  // namespace

// Writes the saved fuzz inputs of the index slack, as the journal inputs are
// written: the byte stream that NtfsFuzz::ProbeOnce() reads from the image, in
// read order and without addresses, with the bytes between the boot sector's
// 4096-byte read and the reader's 512-byte step cut out. The volume is opened
// recovering with deleted items shown, since only then does a scan carve the
// slack. Set NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write them in (the
// repository's NTFSLibTests/fuzz/data) and run this hidden test case by its
// tag.
TEST_CASE("write the index slack fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  const VolumeOptions recovering{.include_deleted = true,
                                 .recover_errors = true};
  for (const Variant& variant : Variants()) {
    const std::filesystem::path path =
        std::filesystem::path(dir) / variant.name;
    {
      NtfsFuzz::ProbeOnce(
          std::make_unique<Tests::RecordingDiskReader>(
              std::make_unique<Tests::MemoryDiskReader>(variant.image), path),
          recovering);
    }
    REQUIRE(std::filesystem::exists(path));

    std::vector<char> stream;
    {
      std::ifstream in(path, std::ios::binary);
      stream.assign(std::istreambuf_iterator<char>(in), {});
    }
    REQUIRE(stream.size() > boot_sector_read_bytes);
    stream.erase(stream.begin() + boot_sector_stream_bytes,
                 stream.begin() + boot_sector_read_bytes);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(stream.data(), static_cast<std::streamsize>(stream.size()));
  }
}
