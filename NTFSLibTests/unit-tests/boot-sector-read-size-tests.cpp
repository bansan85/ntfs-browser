#include <ntfs-browser/win-types.h>

#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

namespace
{

// Sector size of the emulated 4Kn disk: every read at offset 0 MUST be a
// multiple of it.
constexpr size_t kSector4Kn = 4096;

// Wraps an in-memory image and rejects a read at offset 0 whose length the
// emulated medium would refuse. The boot sector is the only such read.
class StrictBootReadReader : public NtfsBrowser::IDiskReader
{
 public:
  // reject decides, from the read length, whether the read at 0 fails.
  StrictBootReadReader(std::vector<BYTE> image, bool (*reject)(size_t length))
      : inner_(std::move(image)), reject_(reject)
  {
  }

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override
  {
    if (addr.QuadPart == 0 && reject_(dest.size()))
    {
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

 private:
  NtfsBrowserTests::MemoryDiskReader inner_;
  bool (*reject_)(size_t length);
};

bool NotMultipleOf4Kn(size_t length) { return length % kSector4Kn != 0; }

bool AtLeast4Kn(size_t length) { return length >= kSector4Kn; }

bool Always(size_t /*length*/) { return true; }

}  // namespace

TEST_CASE(
    "NtfsVolume reads the boot sector in a whole 4Kn sector, so an unbuffered "
    "device accepts it",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<StrictBootReadReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), &NotMultipleOf4Kn);

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
}

TEST_CASE(
    "NtfsVolume still opens a medium too short to serve a whole 4Kn sector",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<StrictBootReadReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), &AtLeast4Kn);

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
}

TEST_CASE("NtfsVolume rejects a volume whose boot sector cannot be read at all",
          "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<StrictBootReadReader>(
      NtfsBrowserTests::BuildFakeNtfsImage(), &Always);

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());
}
