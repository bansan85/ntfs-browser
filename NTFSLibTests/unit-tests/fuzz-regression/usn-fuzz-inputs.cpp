#include <ntfs-browser/win-types.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
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
using Tests::FakeUsnOptions;
using Tests::FakeUsnPatch;
using Tests::FakeUsnRecord;

namespace {

// One saved fuzz input: its file name, and the journal the image holds.
struct Variant {
  std::string name;
  FakeUsnOptions options;
};

// Where a field sits in a V2 record.
constexpr size_t length_offset = 0;
constexpr size_t major_version_offset = 4;
constexpr size_t name_length_offset = 56;

// A major version no journal has.
constexpr WORD unknown_version = 9;

// A journal of count records with names of the same length, so each takes the
// same number of bytes.
std::vector<FakeUsnRecord> Records(size_t count) {
  std::vector<FakeUsnRecord> records;
  for (size_t i = 0; i < count; ++i) {
    records.push_back(
        {.name = L"file" + std::to_wstring(100 + i), .file_record = 100 + i});
  }
  return records;
}

// Offset of record index in a stream of Records(): they all take the same size.
size_t StartOf(size_t index) {
  const size_t size = Tests::EncodeUsnRecord(Records(1).front(), 0).size();
  return index * size;
}

template <class T>
FakeUsnPatch Patch(size_t record_index, size_t field_offset, T value) {
  std::vector<BYTE> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return {.stream_offset = StartOf(record_index) + field_offset,
          .bytes = std::move(bytes)};
}

// The inputs, one per way a hostile $J can be wrong.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;

  FakeUsnOptions forged{.records = Records(6)};
  forged.patches.push_back(Patch<DWORD>(2, length_offset, 4000));
  variants.push_back({"usn_journal_forged_length", std::move(forged)});

  FakeUsnOptions zero{.records = Records(6)};
  zero.patches.push_back(Patch<DWORD>(2, length_offset, 0));
  variants.push_back({"usn_journal_zero_length", std::move(zero)});

  FakeUsnOptions past{.records = Records(6)};
  past.patches.push_back(Patch<WORD>(2, name_length_offset, 2000));
  variants.push_back({"usn_journal_name_past_record", std::move(past)});

  std::vector<FakeUsnRecord> with_v4 = Records(6);
  with_v4.at(2).major_version = 4;
  variants.push_back(
      {"usn_journal_v4_record", {.records = std::move(with_v4)}});

  std::vector<FakeUsnRecord> mixed = Records(5);
  mixed.at(1).major_version = 3;
  mixed.at(2).major_version = 4;
  mixed.at(4).major_version = 3;
  variants.push_back(
      {"usn_journal_mixed_versions", {.records = std::move(mixed)}});

  // Fourteen records fill the first cluster: the last one declares more bytes
  // than are left of it.
  FakeUsnOptions tail{.records = Records(14)};
  tail.patches.push_back(Patch<DWORD>(13, length_offset, 1000));
  variants.push_back({"usn_journal_truncated_tail", std::move(tail)});

  // The first damaged record starts a stretch that only a record whose USN is
  // its own offset ends: the next one has another.
  std::vector<FakeUsnRecord> mismatch = Records(6);
  mismatch.at(3).usn = 777;
  FakeUsnOptions anchor{.records = std::move(mismatch)};
  anchor.patches.push_back(
      Patch<WORD>(2, major_version_offset, unknown_version));
  variants.push_back({"usn_journal_anchor_mismatch", std::move(anchor)});

  variants.push_back(
      {"usn_journal_sparse_head",
       {.records = Records(6), .sparse_head_bytes = ULONGLONG{1} << 30U}});
  return variants;
}

// The freed tree with the variant's journal.
std::vector<BYTE> ImageOf(const Variant& variant) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutUsnJournal(image, variant.options);
  return image;
}

// Bytes of the boot sector read a NoCache volume issues first, and how far the
// fuzzer's reader moves the stream after it.
constexpr size_t boot_sector_read_bytes = 4096;
constexpr size_t boot_sector_stream_bytes = 512;

}  // namespace

// Writes the saved fuzz inputs of the change journal, as the bad cluster inputs
// are written: the byte stream that NtfsFuzz::ProbeOnce() reads from the image,
// in read order and without addresses, with the bytes between the boot sector's
// 4096-byte read and the reader's 512-byte step cut out. The volume is opened
// recovering, since only then does it read the journal. Set
// NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write them in (the repository's
// NTFSLibTests/fuzz/data) and run this hidden test case by its tag.
TEST_CASE("write the usn journal fuzz inputs", "[.write-fuzz-inputs]") {
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
              std::make_unique<Tests::MemoryDiskReader>(ImageOf(variant)),
              path),
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
