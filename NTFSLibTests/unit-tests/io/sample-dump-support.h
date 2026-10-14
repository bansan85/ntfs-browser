#pragma once

// Fixture the ntfsdump and ntfsattr sample tests share.

#include <ntfs-browser/win-types.h>

#include <vector>

#include "fake-ntfs-image.h"

namespace NtfsBrowserTests {

// The dump-tree image with its big file on the default run: 20 clusters at
// LCN 40, past the 16 KiB a dump shows.
inline std::vector<BYTE> DumpTreeImage() {
  return BuildFakeNtfsImageWithDumpTree(
      dump_big_file_size,
      {{.lcn = dump_big_file_lcn, .clusters = dump_big_file_clusters}});
}

}  // namespace NtfsBrowserTests
