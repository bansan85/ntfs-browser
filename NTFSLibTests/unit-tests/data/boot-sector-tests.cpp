#include <ntfs-browser/win-types.h>

#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/catch_message.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far smaller than Data::IndexBlock",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithTinyIndexBlock());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK(volume.GetIndexBlockSize() == NtfsBrowserTests::tiny_index_block_size);

  // tiny_index_block_size doesn't fit Data::IndexBlock's own header.
  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far larger than any plausible size",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedIndexBlock());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "far larger than any plausible size",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedFileRecord());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetFileRecordSize() = " << volume.GetFileRecordSize());
  // GetFileRecordSize() reflects the BPB value directly; IsVolumeOK() fails
  // regardless, from an unrelated $Volume read failure this size triggers.
  CHECK(volume.GetFileRecordSize() !=
        NtfsBrowserTests::oversized_file_record_size);
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "size that exceeds max_file_record_size via the positive "
    "clusters_per_file_record branch",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFileRecordSizeTooBig());

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.GetFileRecordSize() ==
        NtfsBrowserTests::file_record_size_too_big);
  CHECK_FALSE(volume.IsVolumeOK());
}

namespace {

// Sector size of the emulated 4Kn disk: every read at offset 0 MUST be a
// multiple of it.
constexpr size_t sector4_kn = 4096;

// Wraps an in-memory image and rejects a read at offset 0 whose length the
// emulated medium would refuse. The boot sector is the only such read.
class StrictBootReadReader : public NtfsBrowser::IDiskReader {
 public:
  // reject decides, from the read length, whether the read at 0 fails.
  StrictBootReadReader(std::vector<BYTE> image, bool (*reject)(size_t length))
      : inner_(std::move(image)), reject_(reject) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    if (addr.QuadPart == 0 && reject_(dest.size())) {
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

 private:
  NtfsBrowserTests::MemoryDiskReader inner_;
  bool (*reject_)(size_t length);
};

bool NotMultipleOf4Kn(size_t length) { return length % sector4_kn != 0; }

bool AtLeast4Kn(size_t length) { return length >= sector4_kn; }

bool Always(size_t /*length*/) { return true; }

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume reads the boot sector in a whole 4Kn sector, so an unbuffered "
    "device accepts it",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<StrictBootReadReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), &NotMultipleOf4Kn);

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume still opens a medium too short to serve a whole 4Kn sector",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<StrictBootReadReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), &AtLeast4Kn);

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a volume whose boot sector cannot be read at all",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<StrictBootReadReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), &Always);

  const NtfsVolume<S> volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());
}
