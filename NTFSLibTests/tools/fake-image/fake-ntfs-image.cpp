#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string_view>
#include <vector>

#include <ntfs-browser/mft/idx.h>

#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/ntfs-bpb.h"
#include "data/standard-information.h"
#include "data/volume-information.h"
#include "fake-ntfs-image-internal.h"
#include "file-record-header-edit.h"

namespace NtfsBrowserTests {

namespace {

// The boot sector's last two bytes: 0xAA55, little-endian.
constexpr BYTE boot_signature_low = 0xAA;

constexpr BYTE boot_signature_high = 0x55;

// Builds a fake $MFT record: one non-resident DATA attribute whose
// real_size reports sentinel_record_count fake records, with an empty
// data run (GetRecordsCount() only reads real_size).
FakeRecord MakeMftRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::Data;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = 0;
  attr.last_vcn = 0;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = sentinel_record_count * fake_file_record_size;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + run_list_room);

  record.at(attr_offset_value + sizeof(attr)) = 0x00;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a fake $Volume record whose VOLUME_INFORMATION attribute declares
// attrSize bytes, reporting NTFS 3.1.
FakeRecord MakeVolumeRecordSized(WORD attr_size) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::VolumeInformation;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = attr_size;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& vol_info = *reinterpret_cast<NtfsBrowser::Data::VolumeInformation*>(
      &record.at(attr_offset_value + attr.attr_offset));
  vol_info.major_version = 3;
  vol_info.minor_version = 1;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

FakeRecord MakeVolumeRecord() {
  return MakeVolumeRecordSized(
      static_cast<WORD>(sizeof(NtfsBrowser::Data::VolumeInformation)));
}

// Builds a fake $Volume record carrying both VOLUME_INFORMATION (so the
// volume reports NTFS 3.1) and a VOLUME_NAME holding name.
FakeRecord MakeVolumeRecordWithName(std::wstring_view name) {
  FakeRecord record = MakeVolumeRecord();

  const auto& vol_info = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  const DWORD name_offset = attr_offset_value + vol_info.header.total_size;

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(name_offset));
  attr.header.type = Attr::Type::VolumeName;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 1;
  const std::vector<WORD> encoded_name = ToUtf16(name);
  attr.attr_size = gsl::narrow<DWORD>(encoded_name.size() * sizeof(WORD));
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  std::memcpy(&record.at(name_offset + attr.attr_offset), encoded_name.data(),
              attr.attr_size);

  WriteEndOfAttributesMarker(record, name_offset + attr.header.total_size);
  return record;
}

// Builds a resident $STANDARD_INFORMATION attribute exactly attrSize bytes
// long, so a fixture can pin it to NTFS 1.2's real minimum size.
FakeRecord MakeStandardInformationRecordSized(WORD attr_size) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::StandardInformation;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = attr_size;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& std_info = *reinterpret_cast<NtfsBrowser::Data::StandardInformation*>(
      &record.at(attr_offset_value + attr.attr_offset));
  std_info.create_time = std_info_create_time;
  std_info.alter_time = std_info_alter_time;
  std_info.mft_time = std_info_mft_time;
  std_info.read_time = std_info_read_time;
  std_info.permission = NtfsBrowser::Data::StdInfoPermission::ReadOnly;
  std_info.max_version_no = 0;
  std_info.version_no = 0;
  std_info.class_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a bare root-directory record; ParseFileRecord() never looks at
// attributes, so an empty one is enough to exercise a second read.
FakeRecord MakeRootRecord() {
  return MakeRecordHeader(attr_offset_value,
                          NtfsBrowser::Data::FileRecordFlag::InUse |
                              NtfsBrowser::Data::FileRecordFlag::Dir);
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImage() {
  NtfsBrowser::Data::NtfsBpb bpb{};
  std::memcpy(std::data(bpb.signature),
              NtfsBrowser::Data::ntfs_signature.data(), sizeof(bpb.signature));
  bpb.bytes_per_sector = bytes_per_sector;
  bpb.sectors_per_cluster = sectors_per_cluster;
  bpb.lcn_mft = mft_lcn;
  bpb.clusters_per_file_record = 1;
  bpb.clusters_per_index_block = 1;
  bpb.x_aa = boot_signature_low;
  bpb.x_55 = boot_signature_high;

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t records_end =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     (static_cast<size_t>(Mft::Idx::Root) + 1);
  // FullCache always reads a 64 KiB block regardless of length requested.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t image_size = std::max(records_end, full_cache_read_block_size);
  std::vector<BYTE> image(image_size, 0);
  std::memcpy(image.data(), &bpb, sizeof(bpb));

  const auto put_record = [&](Mft::Idx idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * static_cast<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };
  put_record(Mft::Idx::Mft, MakeMftRecord());
  put_record(Mft::Idx::Volume, MakeVolumeRecord());
  put_record(Mft::Idx::Root, MakeRootRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMinimalVolumeInformation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(Mft::Idx::Volume);
  const FakeRecord record =
      MakeVolumeRecordSized(minimal_volume_information_size);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEmptyVolumeInformation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(Mft::Idx::Volume);
  const FakeRecord record = MakeVolumeRecordSized(0);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithVolumeName() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(Mft::Idx::Volume);
  const FakeRecord record = MakeVolumeRecordWithName(fake_volume_name);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithDeletedVolumeRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(Mft::Idx::Volume);

  // Same $VOLUME_INFORMATION content as BuildFakeNtfsImage(), but with the
  // INUSE flag cleared - a freed record. bypass_deleted_gate_ must still let
  // NtfsVolume::Init() read it.
  FakeRecord record = MakeVolumeRecord();
  EditFileRecordHeader(record, [](FileRecordHeader& header) {
    header.flags = NtfsBrowser::Data::FileRecordFlag{};
  });

  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithLegacyStandardInformation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset =
      mft_addr +
      static_cast<size_t>(fake_file_record_size) *
          static_cast<size_t>(legacy_standard_information_record_idx);
  const FakeRecord record =
      MakeStandardInformationRecordSized(legacy_standard_information_size);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEmptyStandardInformation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset =
      mft_addr +
      static_cast<size_t>(fake_file_record_size) *
          static_cast<size_t>(legacy_standard_information_record_idx);
  const FakeRecord record = MakeStandardInformationRecordSized(0);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithLegacyStandardInformationOnRoot() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record =
      MakeStandardInformationRecordSized(legacy_standard_information_size);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptRootRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Zeroes the root directory's (#5) own record, so ParseFileRecord(ROOT)
  // fails while $Volume and $MFT stay valid.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  std::memset(&image.at(root_offset), 0, fake_file_record_size);

  return image;
}

std::filesystem::path WriteFakeNtfsImage() {
  return WriteFakeNtfsImage(BuildFakeNtfsImage());
}

std::filesystem::path WriteFakeNtfsImage(const std::vector<BYTE>& image) {
  std::random_device random_device;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-fake-volume-" + std::to_wstring(random_device()) +
       L".img");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(image.data()),
            gsl::narrow<std::streamsize>(image.size()));

  return path;
}

}  // namespace NtfsBrowserTests
