#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "usn/usn-record-scanner.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Test::DiskReaders {

class CountingDiskReader;
class MemoryDiskReader;

}  // namespace Test::DiskReaders
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Test::Usn {

// The volume options of the journal tests: recovery on, deleted items shown.
inline VolumeOptions RecoverOptions() {
  return VolumeOptions{.include_deleted = true, .recover_errors = true};
}

// What a scan of some bytes found.
struct ScanResult {
  std::vector<NTFS_BROWSER_NAMESPACE::Usn::Record> records;
  ULONGLONG damaged{0};
  ULONGLONG skipped{0};
};

// Scans bytes as one range of $J that starts at stream_offset, in one Feed().
inline ScanResult ScanBytes(std::span<const BYTE> bytes,
                            ULONGLONG stream_offset = 0) {
  NTFS_BROWSER_NAMESPACE::Usn::RecordScanner scanner;
  ScanResult result;
  scanner.Reset();
  scanner.Feed(bytes, stream_offset, result.records);
  scanner.Finish(result.records);
  result.damaged = scanner.DamagedRecords();
  result.skipped = scanner.SkippedRecords();
  return result;
}

// Writes a little-endian value into bytes at offset.
template <class T>
void PutAt(std::vector<BYTE>& bytes, size_t offset, T value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(value));
}

// The time a journal record reports for a UTC value that the fixture wrote:
// converted as the library converts every timestamp it gives out.
inline FILETIME ExpectedTime(ULONGLONG utc) {
  constexpr unsigned dword_bits = 32;
  FILETIME utc_time{.dwLowDateTime = static_cast<DWORD>(utc & 0xFFFFFFFFULL),
                    .dwHighDateTime = static_cast<DWORD>(utc >> dword_bits)};
#ifdef _WIN32
  FILETIME local_time{};
  if (FileTimeToLocalFileTime(&utc_time, &local_time) != 0) {
    return local_time;
  }
#endif
  return utc_time;
}

// The 26-slot MftTree base with a journal written into it.
inline std::vector<BYTE>
    JournalImage(const Test::FakeImage::FakeUsnOptions& options) {
  std::vector<BYTE> image =
      Test::FakeImage::BuildFakeNtfsImageWithFreedFileTree();
  Test::FakeImage::PutUsnJournal(image, options);
  return image;
}

// A volume over image, whose reader counts every call.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
struct Opened {
  std::unique_ptr<NtfsVolume<S>> volume;
  // Owned by volume.
  Test::DiskReaders::CountingDiskReader* counter{nullptr};
};

// Opens image with options, over a counting reader.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
Opened<S> OpenVolume(std::vector<BYTE> image,
                     const VolumeOptions& options = RecoverOptions()) {
  auto counter = std::make_unique<Test::DiskReaders::CountingDiskReader>(
      std::make_unique<Test::DiskReaders::MemoryDiskReader>(std::move(image)));
  Opened<S> opened;
  opened.counter = counter.get();
  opened.volume = std::make_unique<NtfsVolume<S>>(std::move(counter), options);
  REQUIRE(opened.volume->IsVolumeOK());
  return opened;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Usn
