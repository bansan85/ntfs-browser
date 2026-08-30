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

// One sector per file record: the Update Sequence fixup only has a single
// slot then, at the record's last 2 bytes.
constexpr WORD kBytesPerSector = kFakeFileRecordSize;
constexpr BYTE kSectorsPerCluster = 1;
constexpr DWORD kClusterSize = kBytesPerSector * kSectorsPerCluster;
constexpr ULONGLONG kMftLcn = 1;  // $MFT starts right after the boot sector
constexpr WORD kAttrOffset = 48;  // right after FileRecordHeader::Data's header fields

// Point offset_of_us at the fixup slot itself (last 2 bytes of the record):
// FileRecordHeader's ctor then "captures" that same slot as the value to
// restore, so PatchUS() trivially succeeds without a real fixup array.
constexpr WORD kOffsetOfUs = kFakeFileRecordSize - 4;

static_assert(kAttrOffset + sizeof(NtfsBrowser::Attr::HeaderNonResident) + 8 <
                  kOffsetOfUs,
              "attribute data must not reach into the fixup slot");

using FakeRecord = std::array<BYTE, kFakeFileRecordSize>;

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

void WriteEndOfAttributesMarker(FakeRecord& record, DWORD offset)
{
  const DWORD marker = static_cast<DWORD>(AttrType::ALL);
  std::memcpy(&record[offset], &marker, sizeof(marker));
}

// $MFT (#0): a single non-resident DATA attribute whose real_size encodes
// kSentinelRecordCount file records, with an empty (immediately-terminated)
// data run - GetRecordsCount() only reads real_size, never the data run.
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

  // Data run: a single 0x00 byte terminates the run list immediately.
  record[kAttrOffset + sizeof(attr)] = 0x00;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// $Volume (#3): a resident VOLUME_INFORMATION attribute reporting NTFS 3.1,
// the minimum NtfsVolume<S>::Init() requires to accept the volume.
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

// Root directory (#5): a bare, valid file record. ParseFileRecord() never
// looks at attributes, so this is enough to reproduce a second ParseFileRecord
// call taking the same "direct disk allocation" path as $MFT and $Volume.
FakeRecord MakeRootRecord()
{
  return MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                           NtfsBrowser::Flag::FileRecord::DIR);
}

}  // namespace

std::filesystem::path WriteFakeNtfsImage()
{
  NtfsBrowser::Data::NtfsBpb bpb{};
  std::memcpy(bpb.signature, NTFS_SIGNATURE, sizeof(bpb.signature));
  bpb.bytes_per_sector = kBytesPerSector;
  bpb.sectors_per_cluster = kSectorsPerCluster;
  bpb.lcn_mft = kMftLcn;
  bpb.clusters_per_file_record = 1;   // positive sz -> cluster_size * 1
  bpb.clusters_per_index_block = 1;   // unused by these tests
  bpb.x_aa = 0xAA;
  bpb.x_55 = 0x55;

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t recordsEnd =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    (static_cast<size_t>(MftIdx::ROOT) + 1);
  // FULL_CACHE's FileReader::Read() always pulls in a full 64KB block from
  // FILE_BEGIN, regardless of the length actually requested (used for the
  // boot sector read) - the image must be at least that large.
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

  std::random_device rd;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-fake-volume-" + std::to_wstring(rd()) + L".img");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(image.data()),
            static_cast<std::streamsize>(image.size()));

  return path;
}

}  // namespace NtfsBrowserTests
