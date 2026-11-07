#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fuzz/fuzz-core.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/recording-reader.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

std::vector<BYTE>
    RecordedStream::Record(std::vector<BYTE> image,
                           const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                           Probe probe) {
  const std::size_t image_size = image.size();
  std::vector<BYTE> stream;
  {
    auto reader = std::make_unique<RecordingReader>(
        std::make_unique<Test::DiskReaders::MemoryDiskReader>(std::move(image)),
        stream);
    switch (probe) {
      case Probe::Main:
        Test::Fuzz::ProbeOnce(std::move(reader), options);
        break;
      case Probe::Scan:
        Test::Fuzz::ProbeScanOnce(std::move(reader), options);
        break;
      case Probe::ClusterAccount:
        Test::Fuzz::ProbeClusterAccountOnce(std::move(reader), options);
        break;
    }
  }

  if (stream.size() <= boot_sector_read_bytes) {
    throw std::runtime_error("the probe read no more than the boot sector");
  }
  stream.erase(stream.begin() + boot_sector_stream_bytes,
               stream.begin() + boot_sector_read_bytes);
  if (probe == Probe::ClusterAccount && stream.size() < image_size) {
    stream.resize(image_size);
  }
  return stream;
}

NTFS_BROWSER_NAMESPACE::VolumeOptions RecordedStream::Recovering() {
  return {.include_deleted = true, .recover_errors = true};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
