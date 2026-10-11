#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "memory-disk-reader.h"

namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::VolumeOptions;

namespace {

// Fake images place the forged index block at this LCN.
constexpr size_t index_block_lcn = 20;

template <Cache::Strategy S>
struct ParsedRecord {
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<NtfsBrowser::Io::FileRecord<S>> record;
};

// Opens record idx through a volume over reader, not yet attribute-parsed.
template <Cache::Strategy S>
ParsedRecord<S> OpenRecordFrom(std::unique_ptr<NtfsBrowser::IDiskReader> reader,
                               ULONGLONG idx, VolumeOptions options = {}) {
  ParsedRecord<S> parsed;
  parsed.volume = std::make_unique<NtfsVolume<S>>(std::move(reader), options);
  REQUIRE(parsed.volume->IsVolumeOK());

  parsed.record =
      std::make_unique<NtfsBrowser::Io::FileRecord<S>>(*parsed.volume);
  REQUIRE(parsed.record->ParseFileRecord(idx));
  return parsed;
}

// Opens record idx of image through a fresh volume, not yet attribute-parsed.
template <Cache::Strategy S>
ParsedRecord<S> OpenRecord(std::vector<BYTE> image, ULONGLONG idx,
                           VolumeOptions options = {}) {
  return OpenRecordFrom<S>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)),
      idx, options);
}

}  // namespace
