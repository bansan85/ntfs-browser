#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Turns an image into the saved fuzz input of a probe: the byte stream that
// the probe reads from the image, in read order and without addresses. The
// bytes between the 4096-byte read of the boot sector and the 512-byte step of
// the replay reader are cut out (see Test::Fuzz::LoopingDiskReader).
class RecordedStream {
 public:
  // The probe whose reads are recorded.
  enum class Probe : std::uint8_t {
    // Test::Fuzz::ProbeOnce(), the main probes.
    Main,
    // Test::Fuzz::ProbeScanOnce(), the scan that the thread probe repeats with
    // one thread and with several.
    Scan,
    // Test::Fuzz::ProbeClusterAccountOnce(), the scan with the cluster account
    // on. The input is padded with zeros to the size of the image: the replay
    // reader reports the length of the input as the size of the volume, and
    // the account judges clusters against it.
    ClusterAccount
  };

  // The saved input of probe over image, with the volume opened under options.
  // Throws std::runtime_error when the probe reads no more than the boot
  // sector.
  [[nodiscard]] static std::vector<BYTE>
      Record(std::vector<BYTE> image,
             const NTFS_BROWSER_NAMESPACE::VolumeOptions& options, Probe probe);

  // The options most inputs are recorded under: recovering, with deleted items
  // shown. A scan salvages a damaged record and reads the journal only then.
  [[nodiscard]] static NTFS_BROWSER_NAMESPACE::VolumeOptions Recovering();

  // Bytes of the boot sector read that a NoCache volume issues first.
  static constexpr std::size_t boot_sector_read_bytes = 4096;

  // How far the replay reader moves the stream after that read. The saved
  // corpus was built when the boot sector was read in 512 bytes.
  static constexpr std::size_t boot_sector_stream_bytes = 512;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
