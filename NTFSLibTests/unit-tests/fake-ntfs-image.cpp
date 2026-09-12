#include "fake-ntfs-image.h"

#include <array>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>

#include <windows.h>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/flag/file-record.h>
#include <ntfs-browser/mft-idx.h>

#include "attr/header-non-resident.h"
#include "attr/header-resident.h"
#include "attr/volume-information.h"
#include "data/ntfs-bpb.h"

namespace NtfsBrowserTests
{

using NtfsBrowser::AttrType;
using NtfsBrowser::kFileRecordMagic;
using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// One sector per record, so the USN fixup needs only one slot.
constexpr WORD kBytesPerSector = kFakeFileRecordSize;
// One sector per cluster keeps addressing simple for the fake image.
constexpr BYTE kSectorsPerCluster = 1;
constexpr DWORD kClusterSize = kBytesPerSector * kSectorsPerCluster;
// $MFT sits in the first cluster after the boot sector.
constexpr ULONGLONG kMftLcn = 1;
// Right after FileRecordHeader::Data's fixed header fields.
constexpr WORD kAttrOffset = 48;

// Points the fixup slot at the record's own last 4 bytes, so PatchUS()
// succeeds without a real fixup array.
constexpr WORD kOffsetOfUs = kFakeFileRecordSize - 4;

static_assert(kAttrOffset + sizeof(NtfsBrowser::Attr::HeaderNonResident) + 8 <
                  kOffsetOfUs,
              "attribute data must not reach into the fixup slot");

using FakeRecord = std::array<BYTE, kFakeFileRecordSize>;

// Builds a bare file-record header with the given attribute offset and
// flags.
FakeRecord MakeRecordHeader(WORD offsetOfAttr, NtfsBrowser::Flag::FileRecord flags)
{
  FakeRecord record{};

  auto& header = *reinterpret_cast<FileRecordHeader::Data*>(record.data());
  header.magic = kFileRecordMagic;
  header.offset_of_us = kOffsetOfUs;
  header.size_of_us = 2;
  header.offset_of_attr = offsetOfAttr;
  header.flags = flags;

  return record;
}

// Writes the AttrType::ALL end-of-attributes marker at offset.
void WriteEndOfAttributesMarker(FakeRecord& record, DWORD offset)
{
  const DWORD marker = static_cast<DWORD>(AttrType::ALL);
  std::memcpy(&record[offset], &marker, sizeof(marker));
}

// Builds a fake $MFT record: one non-resident DATA attribute whose
// real_size reports kSentinelRecordCount fake records, with an empty
// data run (GetRecordsCount() only reads real_size).
FakeRecord MakeMftRecord()
{
  FakeRecord record = MakeRecordHeader(
      kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = 0;
  attr.last_vcn = 0;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = kSentinelRecordCount * kFakeFileRecordSize;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + 8;

  record[kAttrOffset + sizeof(attr)] = 0x00;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a fake $Volume record: a resident VOLUME_INFORMATION attribute
// reporting NTFS 3.1, the minimum NtfsVolume<S>::Init() accepts.
FakeRecord MakeVolumeRecord()
{
  FakeRecord record = MakeRecordHeader(
      kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::VOLUME_INFORMATION;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = sizeof(NtfsBrowser::Attr::VolumeInformation);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size =
      static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& volInfo = *reinterpret_cast<NtfsBrowser::Attr::VolumeInformation*>(
      &record[kAttrOffset + attr.attr_offset]);
  volInfo.major_version = 3;
  volInfo.minor_version = 1;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a bare root-directory record; ParseFileRecord() never looks at
// attributes, so an empty one is enough to exercise a second read.
FakeRecord MakeRootRecord()
{
  return MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                           NtfsBrowser::Flag::FileRecord::DIR);
}

}

std::vector<BYTE> BuildFakeNtfsImage()
{
  NtfsBrowser::Data::NtfsBpb bpb{};
  std::memcpy(bpb.signature, NTFS_SIGNATURE, sizeof(bpb.signature));
  bpb.bytes_per_sector = kBytesPerSector;
  bpb.sectors_per_cluster = kSectorsPerCluster;
  bpb.lcn_mft = kMftLcn;
  bpb.clusters_per_file_record = 1;
  bpb.clusters_per_index_block = 1;
  bpb.x_aa = 0xAA;
  bpb.x_55 = 0x55;

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t recordsEnd =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    (static_cast<size_t>(MftIdx::ROOT) + 1);
  // FULL_CACHE always reads a 64 KiB block regardless of length requested.
  constexpr size_t kFullCacheReadBlockSize = 64 * 1024;
  const size_t imageSize = max(recordsEnd, kFullCacheReadBlockSize);
  std::vector<BYTE> image(imageSize, 0);
  std::memcpy(image.data(), &bpb, sizeof(bpb));

  const auto putRecord = [&](MftIdx idx, const FakeRecord& record)
  {
    const size_t offset =
        mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                      static_cast<size_t>(idx);
    std::memcpy(image.data() + offset, record.data(), record.size());
  };
  putRecord(MftIdx::MFT, MakeMftRecord());
  putRecord(MftIdx::VOLUME, MakeVolumeRecord());
  putRecord(MftIdx::ROOT, MakeRootRecord());

  return image;
}

std::filesystem::path WriteFakeNtfsImage()
{
  const std::vector<BYTE> image = BuildFakeNtfsImage();

  std::random_device rd;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-fake-volume-" + std::to_wstring(rd()) + L".img");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(image.data()),
            static_cast<std::streamsize>(image.size()));

  return path;
}

}
