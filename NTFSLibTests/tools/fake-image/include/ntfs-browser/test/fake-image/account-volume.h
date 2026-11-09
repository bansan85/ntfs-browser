#pragma once

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "alloc/types.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::FakeImage {

// Clusters of the volume past which a run is out of range: the base image is 64
// clusters, and its bitmap clusters follow.
inline constexpr ULONGLONG account_far_lcn = 5000;

// A volume whose files and bitmap a caller composes, then builds.
struct FakeAccountVolume {
  // The 26-slot MftTree base, whose freed and live files sit at 17 and above.
  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();
  // The clusters the bitmap marks allocated.
  std::vector<FakeDataRun> allocated;
  // The clusters $BadClus lists.
  std::vector<NTFS_BROWSER_NAMESPACE::Alloc::Extent> bad;
  // False leaves $Bitmap out: nothing is known of the allocation.
  bool with_bitmap = true;
  // How the bitmap is written, and how large the volume is.
  FakeBitmapOptions bitmap_options;

  // Writes file at slot idx.
  void Put(ULONGLONG idx, const FakeAccountFile& file);

  // The image with its bitmap and bad list.
  [[nodiscard]] std::vector<BYTE> Build() const;
};

// A stream of the unnamed $DATA of runs, size bytes long, fully written.
[[nodiscard]] FakeAccountStream MakeAccountStream(std::vector<FakeDataRun> runs,
                                                  ULONGLONG size);

// A freed file whose header carries the $LogFile sequence number lsn, with one
// stream.
[[nodiscard]] FakeAccountFile MakeFreedAccountFile(ULONGLONG lsn,
                                                   FakeAccountStream stream);

// A live file with one stream.
[[nodiscard]] FakeAccountFile MakeLiveAccountFile(FakeAccountStream stream);

}  // namespace NTFS_BROWSER_NAMESPACE::Test::FakeImage
