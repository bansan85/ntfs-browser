#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfsundel-core.h"

namespace {

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NoCacheRecord =
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache>;
using NoCacheVolume =
    NTFS_BROWSER_NAMESPACE::NtfsVolume<Cache::Strategy::NoCache>;
using NtfsUndel::CopyResult;
using NtfsUndel::RecoverError;

// Freed record of the non-resident fixture's file.
constexpr std::uint64_t freed_file_ref = 22;

// Freed directory of BuildFakeNtfsImageWithFreedFileTree(). It has no $DATA.
constexpr std::uint64_t freed_dir_ref = 20;

// A volume over image, opened the way the dialog opens it for recovery.
std::unique_ptr<NoCacheVolume> OpenVolume(std::vector<BYTE> image) {
  return std::make_unique<NoCacheVolume>(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          std::move(image)),
      NTFS_BROWSER_NAMESPACE::VolumeOptions{.include_deleted = true});
}

// What a copy did: its result, the size of each chunk it wrote, the bytes
// written, and how often it asked about a large file.
struct CopyRun {
  CopyResult result;
  std::vector<std::size_t> chunk_sizes;
  std::string bytes;
  int confirm_calls = 0;
};

// Copies record's unnamed $DATA. confirm and write_ok script the two hooks.
CopyRun RunCopy(const NoCacheRecord& record, bool confirm = true,
                bool write_ok = true) {
  CopyRun run;
  run.result = NtfsUndel::CopyUnnamedData(
      record,
      [&run, confirm](std::uint64_t) {
        ++run.confirm_calls;
        return confirm;
      },
      [&run, write_ok](std::span<const BYTE> chunk) {
        run.chunk_sizes.push_back(chunk.size());
        run.bytes.append(reinterpret_cast<const char*>(chunk.data()),
                         chunk.size());
        return write_ok;
      });
  return run;
}

}  // namespace

TEST_CASE("NtfsUndel recovery opens a freed record and copies its data",
          "[undel][recover][characterization]") {
  const auto volume = OpenVolume(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithFreedFileTree());

  NtfsUndel::OpenedRecord opened = NtfsUndel::OpenDeletedFile(*volume, 19);
  REQUIRE(opened.error == RecoverError::None);
  REQUIRE(opened.record != nullptr);

  const CopyRun run = RunCopy(*opened.record);

  CHECK(run.result.error == RecoverError::None);
  CHECK(run.result.bytes ==
        NTFS_BROWSER_NAMESPACE::Tests::freed_file_content.size());
  CHECK(run.bytes == NTFS_BROWSER_NAMESPACE::Tests::freed_file_content);
  CHECK(run.chunk_sizes ==
        std::vector<std::size_t>{
            NTFS_BROWSER_NAMESPACE::Tests::freed_file_content.size()});
  CHECK(run.confirm_calls == 0);
}

TEST_CASE("NtfsUndel recovery reports a record that does not parse",
          "[undel][recover][characterization]") {
  const auto volume = OpenVolume(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithFreedFileTree());

  // Record 4 is a zero-filled slot. Its magic does not match.
  NtfsUndel::OpenedRecord opened = NtfsUndel::OpenDeletedFile(*volume, 4);

  CHECK(opened.error == RecoverError::RecordParse);
  CHECK(opened.record == nullptr);
}

TEST_CASE("NtfsUndel recovery reports attributes that do not parse",
          "[undel][recover][characterization]") {
  const auto volume =
      OpenVolume(NTFS_BROWSER_NAMESPACE::Tests::
                     BuildFakeNtfsImageWithUndersizedAttribute());

  NtfsUndel::OpenedRecord opened = NtfsUndel::OpenDeletedFile(
      *volume, NTFS_BROWSER_NAMESPACE::Tests::undersized_attr_record_idx);

  CHECK(opened.error == RecoverError::AttributeParse);
  CHECK(opened.record == nullptr);
}

TEST_CASE("NtfsUndel recovery finds no data stream in a freed directory",
          "[undel][recover][characterization]") {
  const auto volume = OpenVolume(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithFreedFileTree());

  NtfsUndel::OpenedRecord opened =
      NtfsUndel::OpenDeletedFile(*volume, freed_dir_ref);
  REQUIRE(opened.error == RecoverError::None);

  const CopyRun run = RunCopy(*opened.record);

  CHECK(run.result.error == RecoverError::NoDataStream);
  CHECK(run.chunk_sizes.empty());
}

TEST_CASE("NtfsUndel recovery copies a large file in ordered chunks",
          "[undel][recover][characterization]") {
  // 147 sparse clusters of 1 KiB: 150528 bytes, of which 150000 are data.
  const auto volume = OpenVolume(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
          150000,
          {NTFS_BROWSER_NAMESPACE::Tests::FakeDataRun{.clusters = 147}}));

  NtfsUndel::OpenedRecord opened =
      NtfsUndel::OpenDeletedFile(*volume, freed_file_ref);
  REQUIRE(opened.error == RecoverError::None);

  const CopyRun run = RunCopy(*opened.record);

  constexpr std::size_t chunk = NtfsUndel::kCopyChunkSize;
  CHECK(run.result.error == RecoverError::None);
  CHECK(run.result.bytes == 150000);
  CHECK(run.chunk_sizes ==
        std::vector<std::size_t>{chunk, chunk, 150000 - 2 * chunk});
  CHECK(run.bytes == std::string(150000, '\0'));
  CHECK(run.confirm_calls == 0);
}

TEST_CASE("NtfsUndel recovery stops when the caller declines a large file",
          "[undel][recover][characterization]") {
  // One byte past the confirmation size. The runs stop short of that size:
  // nothing is read once the caller declines, and a full run list would not
  // fit one file record.
  const auto volume = OpenVolume(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
          NtfsUndel::kConfirmFileSize + 1,
          {NTFS_BROWSER_NAMESPACE::Tests::FakeDataRun{.clusters = 255}}));

  NtfsUndel::OpenedRecord opened =
      NtfsUndel::OpenDeletedFile(*volume, freed_file_ref);
  REQUIRE(opened.error == RecoverError::None);

  const CopyRun run = RunCopy(*opened.record, /*confirm=*/false);

  CHECK(run.result.error == RecoverError::Declined);
  CHECK(run.confirm_calls == 1);
  CHECK(run.chunk_sizes.empty());
}

TEST_CASE("NtfsUndel recovery reports a failed write and stops there",
          "[undel][recover][characterization]") {
  const auto volume = OpenVolume(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithFreedFileTree());

  NtfsUndel::OpenedRecord opened = NtfsUndel::OpenDeletedFile(*volume, 19);
  REQUIRE(opened.error == RecoverError::None);

  const CopyRun run = RunCopy(*opened.record, /*confirm=*/true,
                              /*write_ok=*/false);

  CHECK(run.result.error == RecoverError::Write);
  CHECK(run.chunk_sizes.size() == 1);
}

TEST_CASE("NtfsUndel recovery reports a read the disk cannot serve",
          "[undel][recover][characterization]") {
  // The run maps far past the end of the image, so its read fails.
  const auto volume = OpenVolume(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
          1024, {NTFS_BROWSER_NAMESPACE::Tests::FakeDataRun{.lcn = 1000000U,
                                                            .clusters = 1}}));

  NtfsUndel::OpenedRecord opened =
      NtfsUndel::OpenDeletedFile(*volume, freed_file_ref);
  REQUIRE(opened.error == RecoverError::None);

  const CopyRun run = RunCopy(*opened.record);

  CHECK(run.result.error == RecoverError::Read);
  CHECK(run.chunk_sizes.empty());
}
