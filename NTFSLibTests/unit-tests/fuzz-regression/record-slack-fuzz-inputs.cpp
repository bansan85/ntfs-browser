#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
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

// Bytes the fake record of the Docs directory keeps after its terminator,
// before the fixup array: from offset 8 after the end marker to byte 1017.
constexpr size_t docs_slack_room = 490;

// The eight bytes NTFS writes at the end of the attributes, as they sit in
// every real record.
constexpr std::array<BYTE, 8> terminator{0xFF, 0xFF, 0xFF, 0xFF,
                                         0x82, 0x79, 0x47, 0x11};

FakeSlackName Old(std::wstring name) {
  return FakeSlackName{.name = std::move(name), .with_entry_header = true};
}

// The freed tree with bytes written into the slack of the Docs directory.
std::vector<BYTE> ImageWith(std::span<const BYTE> slack) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutRecordSlack(image, Tests::mft_tree_docs_idx, 8,
                        slack.first(std::min(slack.size(), docs_slack_room)));
  return image;
}

// Candidates on top of each other: the same structure written at every even
// offset, each copy cutting the one before it.
std::vector<BYTE> DenseNames() {
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(Old(L"dense.txt"));
  std::vector<BYTE> slack(docs_slack_room);
  for (size_t offset = 0; offset + bytes.size() <= slack.size(); offset += 2) {
    std::ranges::copy(bytes,
                      slack.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return slack;
}

// A name of 255 characters whose end the record's last byte cuts off, after a
// name that fits.
std::vector<BYTE> NameRunsPastTheRecord() {
  const std::vector<BYTE> bytes =
      Tests::EncodeSlackName(Old(std::wstring(255, L'n')));
  std::vector<BYTE> slack = Tests::EncodeSlackName(Old(L"keeper.txt"));
  slack.resize(docs_slack_room);
  const size_t start = slack.size() - 200;
  std::ranges::copy(std::span(bytes).first(200),
                    slack.begin() + static_cast<std::ptrdiff_t>(start));
  return slack;
}

// What an index root that shrank several times leaves: names separated by old
// terminators, one of them with its parent reference cut by the marker.
std::vector<BYTE> StaleTerminators() {
  std::vector<BYTE> slack;
  const auto append = [&slack](std::span<const BYTE> bytes) {
    slack.insert(slack.end(), bytes.begin(), bytes.end());
  };
  append(Tests::EncodeSlackName(Old(L"first-old.txt")));
  append(terminator);
  append(Tests::EncodeSlackName({.name = L"second-old.txt",
                                 .parent_record = 0xFFFFFFFF,
                                 .with_entry_header = false}));
  append(terminator);
  append(Tests::EncodeSlackName(Old(L"third-old.txt")));
  append(terminator);
  slack.resize(std::min(slack.size(), docs_slack_room));
  return slack;
}

// The inputs, one per way a hostile slack can be wrong.
std::vector<Variant> Variants() {
  return {{"record_slack_dense_names", ImageWith(DenseNames())},
          {"record_slack_name_runs_past_record",
           ImageWith(NameRunsPastTheRecord())},
          {"record_slack_stale_terminators", ImageWith(StaleTerminators())}};
}

// Bytes of the boot sector read a NoCache volume issues first, and how far the
// fuzzer's reader moves the stream after it.
constexpr size_t boot_sector_read_bytes = 4096;
constexpr size_t boot_sector_stream_bytes = 512;

}  // namespace

// Writes the saved fuzz inputs of the record slack, as the journal inputs are
// written: the byte stream that NtfsFuzz::ProbeOnce() reads from the image, in
// read order and without addresses, with the bytes between the boot sector's
// 4096-byte read and the reader's 512-byte step cut out. The volume is opened
// recovering with deleted items shown, since only then does a scan carve the
// slack. Set NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write them in (the
// repository's NTFSLibTests/fuzz/data) and run this hidden test case by its
// tag.
TEST_CASE("write the record slack fuzz inputs", "[.write-fuzz-inputs]") {
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
