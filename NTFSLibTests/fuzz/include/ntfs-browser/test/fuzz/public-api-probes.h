#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <ntfs-browser/namespace.h>

#include "disk-reader.h"
#include "volume-options.h"
#include "win-types.h"

// The probes of FuzzTestcase() that use the public API of the library: Scan
// and Reader. They open the volume themselves, from a target string, so they
// reach the input through the reader factory of the tests. They read in an
// order of their own and never share a reader with the other probes.
namespace NTFS_BROWSER_NAMESPACE::Test::Fuzz {

// How much of the public API one run of ProbePublicApi() covers.
enum class ApiDepth : std::uint8_t {
  // The three operations of ApiProbe, one volume each. Enough to meet a failing
  // read at every stage of the open.
  Short,
  // Every engine, the patterns, the filters, the targets that fail, and the
  // Reader on every call.
  Full
};

// Lists the input with Scan, then reads the files it found with Reader, with
// the volume opened under the flags of options. failing_read makes that one
// read of each volume fail. A thrown exception counts as a rejected input.
void ProbePublicApi(std::span<const BYTE> data,
                    const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                    std::optional<std::size_t> failing_read, ApiDepth depth);

// Lists a file that holds the input, as the scan of a target file does: through
// the readers of the host, with no seam. The bytes are a stream, not an image,
// so the volume mostly fails to open or to read. What the probe reaches is the
// reader that opens the file and the errors it meets.
void ProbeFileTarget(std::span<const BYTE> data,
                     const NTFS_BROWSER_NAMESPACE::VolumeOptions& options);

// One operation of ProbePublicApi() on one volume: what a saved input records.
enum class ApiProbe : std::uint8_t {
  // The scan of the whole volume in $MFT order.
  MftListScan,
  // The recursive walk of the whole volume through the directory index.
  DirectoryScan,
  // The Reader, on the records it reads by number.
  Reader,
  // The scan of the folder known_folder in the $MFT order.
  MftListFolder,
  // The recursive walk of the folder known_folder through the directory index.
  DirectoryFolder
};

// The folder of the volume that the folder probes enter. The fake volumes of
// the saved inputs name one folder so.
inline constexpr std::u16string_view known_folder = u"Docs";

// Runs probe on the volume that reader serves, opened under the flags of
// options. The volume is the only one the probe opens. The same operation runs
// inside ProbePublicApi(), so an input recorded here replays in step there.
void ProbeApiOnce(std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader,
                  const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                  ApiProbe probe);

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Fuzz
