#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "fake-ntfs-image-internal.h"

namespace NtfsBrowserTests {

namespace {

// Writes a resident $EFS attribute holding "body" and returns its total_size.
DWORD WriteResidentEfsAttr(FakeRecord& record, DWORD offset,
                           std::span<const BYTE> body) {
  constexpr std::wstring_view name_value = L"$EFS";
  const std::vector<WORD> encoded_name = ToUtf16(name_value);
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(&record.at(offset));
  attr.header.type = Attr::Type::LoggedUtilityStream;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.header.name_length = gsl::narrow<BYTE>(encoded_name.size());
  attr.header.name_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = gsl::narrow<DWORD>(body.size());
  attr.attr_offset =
      gsl::narrow<WORD>(sizeof(attr) + (encoded_name.size() * sizeof(WORD)));
  attr.header.total_size = AlignAttrSize(attr.attr_offset + attr.attr_size);

  std::memcpy(&record.at(offset + attr.header.name_offset), encoded_name.data(),
              encoded_name.size() * sizeof(WORD));
  std::memcpy(&record.at(offset + attr.attr_offset), body.data(), body.size());
  return attr.header.total_size;
}

}  // namespace

std::vector<BYTE>
    BuildFakeNtfsImageWithEncryptedFile(const FakeEncryptedFile& file) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset,
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Encrypted);

  for (const FakeEncryptedStream& stream : file.streams) {
    WORD flags = stream.flagged_encrypted ? NtfsBrowser::Attr::flag_encrypted
                                          : static_cast<WORD>(0);
    if (stream.flagged_compressed) {
      flags |= NtfsBrowser::Attr::flag_compressed;
    }
    offset +=
        WriteNonResidentAttr(record, offset, Attr::Type::Data,
                             {.comp_unit_size = 0,
                              .real_size = stream.real_size,
                              .runs = stream.runs,
                              .overrides = {.name = stream.name,
                                            .flags = flags,
                                            .ini_size = stream.ini_size}});
  }

  std::vector<FakeDataRun> efs_runs;
  if (!file.efs_stream.empty()) {
    if (file.efs_resident) {
      const DWORD efs_offset = offset;
      offset += WriteResidentEfsAttr(record, offset, file.efs_stream);
      if (file.efs_body_overruns) {
        // attr_offset + attr_size now reaches past total_size.
        constexpr DWORD overrun = 64;
        reinterpret_cast<NtfsBrowser::Data::HeaderResident&>(
            record.at(efs_offset))
            .attr_size += overrun;
      }
    } else {
      efs_runs.push_back(
          {fake_efs_stream_lcn,
           gsl::narrow<DWORD>((file.efs_stream.size() + cluster_size - 1) /
                              cluster_size)});
      offset +=
          WriteNonResidentAttr(record, offset, Attr::Type::LoggedUtilityStream,
                               {.comp_unit_size = 0,
                                .real_size = file.efs_stream.size(),
                                .runs = efs_runs,
                                .overrides = {.name = L"$EFS"}});
    }
  }
  if (file.trailing_undersized_attribute) {
    WriteTrailingDefect(record, offset, FakeTrailingDefect::UndersizedHeader);
  } else {
    WriteEndOfAttributesMarker(record, offset);
  }

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  for (const FakeEncryptedStream& stream : file.streams) {
    LayRunBytes(image, stream.runs, stream.cluster_bytes);
  }
  LayRunBytes(image, efs_runs, file.efs_stream);

  return image;
}

namespace {

// Builds a root-directory replacement whose sole attribute is a RESIDENT
// $DATA carrying the EFS "encrypted" attribute-header flag. Real NTFS never
// encrypts a resident stream (EFS only ever leaves file data non-resident),
// but AttachEfsContext() must still handle a forged one.
FakeRecord MakeResidentEncryptedDataRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::Data;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = NtfsBrowser::Attr::flag_encrypted;
  attr.header.id = 0;
  attr.attr_size = gsl::narrow<DWORD>(resident_encrypted_data_content.size());
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  std::memcpy(&record.at(attr_offset_value + attr.attr_offset),
              resident_encrypted_data_content.data(),
              resident_encrypted_data_content.size());

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithResidentEncryptedData() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeResidentEncryptedDataRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEncryptedDirectory() {
  const std::array<FakeIndexName, 1> root_names{
      {{encrypted_directory_names.at(0), encrypted_directory_mft_refs.at(0),
        false}}};
  const std::array<FakeIndexName, 1> block_names{
      {{encrypted_directory_names.at(1), encrypted_directory_mft_refs.at(1),
        true}}};

  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Encrypted,
      {.comp_unit_size = 0, .real_size = fake_file_record_size, .runs = runs},
      root_names);

  return BuildCompressionImage(record, runs,
                               MakeIndexBlockContent(block_names));
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedEncryptedDirectory() {
  const std::array<FakeIndexName, 1> root_names{
      {{encrypted_directory_names.at(0), encrypted_directory_mft_refs.at(0),
        false}}};
  const std::array<FakeIndexName, 1> block_names{
      {{encrypted_directory_names.at(1), encrypted_directory_mft_refs.at(1),
        true}}};

  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed |
          NtfsBrowser::Data::StdInfoPermission::Encrypted,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs},
      root_names);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeIndexBlockContent(block_names)));
}

}  // namespace NtfsBrowserTests
