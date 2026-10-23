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

// Writes the saved fuzz input of image to path: the byte stream that
// NtfsFuzz::ProbeOnce() reads from it under options, in read order and without
// addresses, with the bytes between the boot sector's 4096-byte read and the
// reader's 512-byte step cut out (see LoopingDiskReader).
inline void WriteRecordedInput(const std::filesystem::path& path,
                               std::vector<BYTE> image,
                               const VolumeOptions& options) {
  {
    NtfsFuzz::ProbeOnce(
        std::make_unique<RecordingDiskReader>(
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

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::FuzzInputs
