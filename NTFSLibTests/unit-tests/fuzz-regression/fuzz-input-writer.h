#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "fuzz-core.h"
#include "memory-disk-reader.h"
#include "ntfs-browser/disk-reader.h"
#include "recording-disk-reader.h"

namespace NTFS_BROWSER_NAMESPACE {

struct VolumeOptions;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Tests::FuzzInputs {

// Bytes of the boot sector read a NoCache volume issues first, and how far the
// fuzzer's reader moves the stream after it.
inline constexpr std::size_t boot_sector_read_bytes = 4096;
inline constexpr std::size_t boot_sector_stream_bytes = 512;

// Writes the saved fuzz input of image to path: the byte stream that probe
// reads from it under options, in read order and without addresses, with the
// bytes between the boot sector's 4096-byte read and the reader's 512-byte step
// cut out (see LoopingDiskReader).
template <class Probe>
void WriteRecorded(const std::filesystem::path& path, std::vector<BYTE> image,
                   const VolumeOptions& options, Probe probe) {
  {
    probe(std::make_unique<RecordingDiskReader>(
              std::make_unique<MemoryDiskReader>(std::move(image)), path),
          options);
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

// WriteRecorded() for the reads of NtfsFuzz::ProbeOnce(), the main probes.
inline void WriteRecordedInput(const std::filesystem::path& path,
                               std::vector<BYTE> image,
                               const VolumeOptions& options) {
  WriteRecorded(
      path, std::move(image), options,
      [](std::unique_ptr<IDiskReader> reader, const VolumeOptions& probed) {
        NtfsFuzz::ProbeOnce(std::move(reader), probed);
      });
}

// WriteRecorded() for the reads of NtfsFuzz::ProbeScanOnce(), the scan that
// the thread probe repeats with one thread and with several.
inline void WriteRecordedScanInput(const std::filesystem::path& path,
                                   std::vector<BYTE> image,
                                   const VolumeOptions& options) {
  WriteRecorded(
      path, std::move(image), options,
      [](std::unique_ptr<IDiskReader> reader, const VolumeOptions& probed) {
        NtfsFuzz::ProbeScanOnce(std::move(reader), probed);
      });
}

// WriteRecorded() for the reads of NtfsFuzz::ProbeClusterAccountOnce(), the
// scan with the cluster account on that the account probe repeats with one
// thread and with several. The input is then padded with zeros to the size of
// the image: the replay reader reports the length of the input as the size of
// the volume, and the account judges clusters against it, so a shorter input
// would put the clusters it reads out of the volume.
inline void WriteRecordedAccountInput(const std::filesystem::path& path,
                                      std::vector<BYTE> image,
                                      const VolumeOptions& options) {
  const std::size_t image_size = image.size();
  WriteRecorded(
      path, std::move(image), options,
      [](std::unique_ptr<IDiskReader> reader, const VolumeOptions& probed) {
        NtfsFuzz::ProbeClusterAccountOnce(std::move(reader), probed);
      });
  if (std::filesystem::file_size(path) < image_size) {
    std::filesystem::resize_file(path, image_size);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::FuzzInputs
