#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/example/ntfsundel/ntfsundel-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>

namespace ExampleNtfsUndel = NTFS_BROWSER_NAMESPACE::Example::NtfsUndel;

namespace {

namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
using ExampleNtfsUndel::CopyResult;
using ExampleNtfsUndel::RecoverError;
using NTFS_BROWSER_NAMESPACE::Test::Io::TempImage;

// Freed record of the non-resident fixture's file.
constexpr std::uint64_t freed_file_ref = 22;

// Freed directory of BuildFakeNtfsImageWithFreedFileTree(). It has no $DATA.
constexpr std::uint64_t freed_dir_ref = 20;

// An image on disk and a Reader over it.
struct Volume {
  std::unique_ptr<TempImage> image;
  std::unique_ptr<Lib::Reader> reader;
};

Volume OpenVolume(const std::vector<BYTE>& image) {
  Volume volume;
  volume.image = std::make_unique<TempImage>(image);
  volume.reader = std::make_unique<Lib::Reader>(Lib::ReaderOptions{
      .target = ExampleNtfsUndel::ToUtf16(volume.image->path.wstring())});
  REQUIRE(volume.reader->IsOpen());
  return volume;
}

// What a copy did: its result, the size of each chunk it wrote, the bytes
// written, and how often it asked about a large file.
struct CopyRun {
  CopyResult result;
  std::vector<std::size_t> chunk_sizes;
  std::string bytes;
  int confirm_calls = 0;
};

// Copies the unnamed $DATA of the record. The handle takes any sequence number.
// confirm and write_ok script the two hooks.
CopyRun RunCopy(Volume& volume, std::uint64_t record, bool confirm = true,
                bool write_ok = true) {
  CopyRun run;
  run.result = ExampleNtfsUndel::CopyUnnamedData(
      *volume.reader, {.record = record},
      [&run, confirm](std::uint64_t) {
        ++run.confirm_calls;
        return confirm;
      },
      [&run, write_ok](std::span<const std::uint8_t> chunk) {
        run.chunk_sizes.push_back(chunk.size());
        run.bytes.append(reinterpret_cast<const char*>(chunk.data()),
                         chunk.size());
        return write_ok;
      });
  return run;
}

}  // namespace

TEST_CASE("NtfsUndel recovery copies the data of a freed record",
          "[undel][recover][characterization]") {
  Volume volume =
      OpenVolume(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const CopyRun run = RunCopy(volume, 19);

  CHECK(run.result.error == RecoverError::None);
  CHECK(run.result.bytes == TestFakeImage::freed_file_content.size());
  CHECK(run.bytes == TestFakeImage::freed_file_content);
  CHECK(run.chunk_sizes ==
        std::vector<std::size_t>{TestFakeImage::freed_file_content.size()});
  CHECK(run.confirm_calls == 0);
}

TEST_CASE("NtfsUndel recovery reports a record that does not parse",
          "[undel][recover][characterization]") {
  Volume volume =
      OpenVolume(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  // Record 4 is a zero-filled slot. Its magic does not match.
  const CopyRun run = RunCopy(volume, 4);

  CHECK(run.result.error == RecoverError::Unreadable);
  CHECK(run.chunk_sizes.empty());
}

TEST_CASE("NtfsUndel recovery reports attributes that do not parse",
          "[undel][recover][characterization]") {
  Volume volume =
      OpenVolume(TestFakeImage::BuildFakeNtfsImageWithUndersizedAttribute());

  const CopyRun run =
      RunCopy(volume, TestFakeImage::undersized_attr_record_idx);

  CHECK(run.result.error == RecoverError::Unreadable);
  CHECK(run.chunk_sizes.empty());
}

TEST_CASE("NtfsUndel recovery finds no data stream in a freed directory",
          "[undel][recover][characterization]") {
  Volume volume =
      OpenVolume(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const CopyRun run = RunCopy(volume, freed_dir_ref);

  CHECK(run.result.error == RecoverError::NoDataStream);
  CHECK(run.chunk_sizes.empty());
}

TEST_CASE("NtfsUndel recovery copies a large file in ordered chunks",
          "[undel][recover][characterization]") {
  // 147 sparse clusters of 1 KiB: 150528 bytes, of which 150000 are data.
  Volume volume =
      OpenVolume(TestFakeImage::BuildFakeNtfsImageWithFreedNonResidentFile(
          150000, {TestFakeImage::FakeDataRun{.clusters = 147}}));

  const CopyRun run = RunCopy(volume, freed_file_ref);

  constexpr std::size_t chunk = Lib::copy_chunk_bytes;
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
  Volume volume =
      OpenVolume(TestFakeImage::BuildFakeNtfsImageWithFreedNonResidentFile(
          ExampleNtfsUndel::kConfirmFileSize + 1,
          {TestFakeImage::FakeDataRun{.clusters = 255}}));

  const CopyRun run = RunCopy(volume, freed_file_ref, /*confirm=*/false);

  CHECK(run.result.error == RecoverError::Declined);
  CHECK(run.confirm_calls == 1);
  CHECK(run.chunk_sizes.empty());
}

TEST_CASE("NtfsUndel recovery reports a failed write and stops there",
          "[undel][recover][characterization]") {
  Volume volume =
      OpenVolume(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const CopyRun run = RunCopy(volume, 19, /*confirm=*/true,
                              /*write_ok=*/false);

  CHECK(run.result.error == RecoverError::Write);
  CHECK(run.chunk_sizes.size() == 1);
}

TEST_CASE("NtfsUndel recovery reports a read the disk cannot serve",
          "[undel][recover][characterization]") {
  // The run maps far past the end of the image, so its read fails.
  Volume volume =
      OpenVolume(TestFakeImage::BuildFakeNtfsImageWithFreedNonResidentFile(
          1024, {TestFakeImage::FakeDataRun{.lcn = 1000000U, .clusters = 1}}));

  const CopyRun run = RunCopy(volume, freed_file_ref);

  CHECK(run.result.error == RecoverError::Read);
  CHECK(run.chunk_sizes.empty());
}
