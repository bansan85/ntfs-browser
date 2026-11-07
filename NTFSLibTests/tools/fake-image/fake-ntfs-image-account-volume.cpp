#include <ntfs-browser/win-types.h>

#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/account-volume.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

namespace NTFS_BROWSER_NAMESPACE::Test::FakeImage {

void FakeAccountVolume::Put(ULONGLONG idx, const FakeAccountFile& file) {
  PutAccountFile(image, idx, file);
}

std::vector<BYTE> FakeAccountVolume::Build() const {
  std::vector<BYTE> built = image;
  if (with_bitmap) {
    PutClusterBitmap(built, AllocatedBitmapFor(built, allocated),
                     bitmap_options);
  }
  if (!bad.empty()) {
    PutBadClusterList(built, bad);
  }
  return built;
}

FakeAccountStream MakeAccountStream(std::vector<FakeDataRun> runs,
                                    ULONGLONG size) {
  FakeAccountStream stream;
  stream.runs = std::move(runs);
  stream.real_size = size;
  return stream;
}

FakeAccountFile MakeFreedAccountFile(ULONGLONG lsn, FakeAccountStream stream) {
  FakeAccountFile file;
  file.freed = true;
  file.lsn = lsn;
  file.streams.push_back(std::move(stream));
  return file;
}

FakeAccountFile MakeLiveAccountFile(FakeAccountStream stream) {
  FakeAccountFile file;
  file.freed = false;
  file.streams.push_back(std::move(stream));
  return file;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::FakeImage
