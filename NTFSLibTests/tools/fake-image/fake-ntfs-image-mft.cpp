#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gsl/narrow>
#include <gsl/util>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/attribute-list.h"
#include "data/file-record-header.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "ntfs-browser/attr/header-common.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Builds $MFT's base record: a resident $ATTRIBUTE_LIST with one entry per
// (extension record index, start VCN) pair, then $MFT's own DATA attribute,
// whose last VCN is baseLastVcn. Every entry carries entrySequence as the
// sequence number of the extension record it names.
FakeRecord MakeMftRecordWithDataContinuations(
    std::span<const std::pair<ULONGLONG, ULONGLONG>> continuations,
    ULONGLONG base_last_vcn = 0, WORD entry_sequence = 0) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);

  DWORD offset = attr_offset_value;

  const auto entry_size = AlignAttrSize(
      NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size);

  auto& list_attr =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderResident*>(
          &record.at(offset));
  list_attr.header.type = Attr::Type::AttributeList;
  list_attr.header.non_resident = 0;
  list_attr.header.name_length = 0;
  list_attr.header.flags = 0;
  list_attr.header.id = 0;
  list_attr.attr_size = entry_size * gsl::narrow<DWORD>(continuations.size());
  list_attr.attr_offset = static_cast<WORD>(sizeof(list_attr));
  list_attr.header.total_size =
      AlignAttrSize(sizeof(list_attr) + list_attr.attr_size);

  for (size_t i = 0; i < continuations.size(); i++) {
    auto& al_entry =
        *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::AttributeList*>(
            &record.at(offset + list_attr.attr_offset + i * entry_size));
    al_entry.attr_type = Attr::Type::Data;
    al_entry.record_size = gsl::narrow<WORD>(entry_size);
    al_entry.name_length = 0;
    al_entry.name_offset = 0;
    al_entry.start_vcn =
        gsl::at(continuations, gsl::narrow<gsl::index>(i)).second;
    al_entry.base_ref.segment_number =
        gsl::at(continuations, gsl::narrow<gsl::index>(i)).first;
    al_entry.base_ref.sequence_number = entry_sequence;
    al_entry.attr_id = 0;
  }

  offset += list_attr.header.total_size;

  auto& data_attr =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
          &record.at(offset));
  data_attr.header.type = Attr::Type::Data;
  data_attr.header.non_resident = 1;
  data_attr.header.name_length = 0;
  data_attr.header.flags = 0;
  data_attr.header.id = 0;
  data_attr.start_vcn = 0;
  data_attr.last_vcn = base_last_vcn;
  data_attr.data_run_offset = static_cast<WORD>(sizeof(data_attr));
  data_attr.comp_unit_size = 0;
  data_attr.real_size = fake_file_record_size;
  data_attr.alloc_size = data_attr.real_size;
  data_attr.ini_size = data_attr.real_size;
  data_attr.header.total_size =
      AlignAttrSize(sizeof(data_attr) + run_list_room);

  record.at(offset + sizeof(data_attr)) = 0x00;

  WriteEndOfAttributesMarker(record, offset + data_attr.header.total_size);
  return record;
}

// Extension record holding the continuation instance of $MFT's own DATA
// attribute: a single non-resident run of clusters clusters at LCN lcn,
// starting at VCN startVcn - unlike MakeMftRecordWithRealDataRun()'s, which
// always starts at VCN 0. It belongs to $MFT (base record 0) and carries
// sequence number sequence.
FakeRecord MakeMftDataContinuationExtensionRecord(ULONGLONG start_vcn,
                                                  DWORD lcn, DWORD clusters,
                                                  WORD sequence = 0) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  SetRecordLink(record, sequence, static_cast<ULONGLONG>(Mft::Idx::Mft));

  auto& attr =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
          &record.at(attr_offset_value));
  attr.header.type = Attr::Type::Data;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = start_vcn;
  attr.last_vcn = start_vcn + clusters - 1;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = (start_vcn + clusters) * cluster_size;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(clusters);
  std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
  run_len += sizeof(lcn);
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  attr.header.total_size = AlignAttrSize(sizeof(attr) + run_len);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a record with valid magic but offset_of_us == fake_file_record_size,
// which Record::Header's ctor rejects outright.
FakeRecord MakeInvalidOffsetOfUsRecord() {
  FakeRecord record{};

  EditFileRecordHeader(record, [](FileRecordHeader& header) {
    header.magic = FileRecordHeader::file_record_magic;
    header.offset_of_us = fake_file_record_size;
    header.size_of_us = 3;
  });

  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithOversizedFileRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_file_record = oversized_clusters_per_file_record;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFileRecordSizeTooBig() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_file_record =
      file_record_size_too_big_clusters_per_file_record;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeMftLcn() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // lcn_mft is otherwise only ever set once, in BuildFakeNtfsImage() itself.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.lcn_mft = huge_mft_lcn;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptMftRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Zeroing the magic makes ParseFileRecord() fail on this record alone.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(Mft::Idx::Mft);
  std::memset(&image.at(offset), 0, fake_file_record_size);

  return image;
}

namespace {

// Breaks the record at offset in image.
void DamageRecordAt(std::vector<BYTE>& image, size_t offset,
                    MirrorRecordDamage kind) {
  if (kind == MirrorRecordDamage::Magic) {
    std::memset(&image.at(offset), 0,
                sizeof(FileRecordHeader::file_record_magic));
    return;
  }
  // The fake records keep their update sequence number at zero.
  constexpr size_t first_sector_end = fake_bytes_per_sector - sizeof(WORD);
  std::memset(&image.at(offset + first_sector_end), 0xFF, sizeof(WORD));
}

}  // namespace

std::vector<BYTE>
    BuildFakeNtfsImageWithMftMirror(const MftMirrorDamage& damage) {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const DWORD mirror_addr =
      static_cast<DWORD>(fake_mft_mirror_lcn) * cluster_size;

  const FakeRecord plain = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::MftMirr),
               plain);
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::LogFile),
               plain);

  const size_t copy_size = fake_file_record_size * fake_mft_mirror_records;
  std::memcpy(&image.at(mirror_addr), &image.at(mft_addr), copy_size);
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.lcn_mft_mirr = fake_mft_mirror_lcn;

  for (ULONGLONG record = 0; record < fake_mft_mirror_records; record++) {
    const size_t offset = fake_file_record_size * record;
    if (((damage.in_mft >> record) & 1U) != 0) {
      DamageRecordAt(image, mft_addr + offset, damage.kind);
    }
    if (((damage.in_mirror >> record) & 1U) != 0) {
      DamageRecordAt(image, mirror_addr + offset, damage.kind);
    }
  }
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedMftInvalidRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // VCN 0..fragmented_mft_invalid_record_idx, inclusive.
  constexpr DWORD clusters_value =
      static_cast<DWORD>(fragmented_mft_invalid_record_idx) + 1;

  // Overwrites $MFT's own record with one whose DATA attribute has a real
  // data run.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const FakeRecord mft_record =
      MakeMftRecordWithRealDataRun(fragmented_mft_data_run_lcn, clusters_value);
  std::memcpy(&image.at(mft_addr), mft_record.data(), mft_record.size());

  // Written at the physical cluster the data run maps this record's VCN to.
  const size_t forged_offset =
      (static_cast<size_t>(fragmented_mft_data_run_lcn) +
       fragmented_mft_invalid_record_idx) *
      cluster_size;
  if (image.size() < forged_offset + fake_file_record_size) {
    image.resize(forged_offset + fake_file_record_size, 0);
  }
  const FakeRecord forged_record = MakeInvalidOffsetOfUsRecord();
  std::memcpy(&image.at(forged_offset), forged_record.data(),
              forged_record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithForgedDirectRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  PutMftRecord(image, mft_addr, forged_direct_record_idx,
               MakeInvalidOffsetOfUsRecord());
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  // Overwrites $MFT's own record: a $ATTRIBUTE_LIST relocating its DATA
  // continuation (starting at mft_data_split_target_idx) to the extension
  // record below.
  const std::array<std::pair<ULONGLONG, ULONGLONG>, 1> continuations{
      {{mft_data_split_ext_idx, mft_data_split_target_idx}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftRecordWithDataContinuations(continuations));

  // Extension record: the continuation instance itself, one cluster
  // starting at VCN mft_data_split_target_idx, mapped to mft_data_split_lcn.
  PutMftRecord(image, mft_addr, mft_data_split_ext_idx,
               MakeMftDataContinuationExtensionRecord(mft_data_split_target_idx,
                                                      mft_data_split_lcn, 1));

  // The target file record itself, at the physical LCN the continuation
  // instance's data run maps its VCN to - only reachable by consulting that
  // instance, since mft_data_split_target_idx is past Mft::Idx::User.
  FakeRecord target_record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  WriteEndOfAttributesMarker(target_record, attr_offset_value);
  PutRecordAt(image, static_cast<size_t>(mft_data_split_lcn) * cluster_size,
              target_record);

  return image;
}

namespace {

// LCN the non-resident $ATTRIBUTE_LIST of
// BuildFakeNtfsImageWithNonResidentMftAttributeList() maps its one cluster to.
// Clear of every other $MFT fixture's LCNs.
constexpr DWORD mft_list_lcn = 50;

// $MFT's base record as MakeMftRecordWithDataContinuations() builds it for
// mft_data_split_target_idx, except its $ATTRIBUTE_LIST is non-resident: one
// cluster at mft_list_lcn, which holds the entries.
FakeRecord MakeMftRecordWithNonResidentAttributeList() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  const DWORD entry_size = AlignAttrSize(
      NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size);

  DWORD offset = attr_offset_value;
  auto& list_attr =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
          &record.at(offset));
  list_attr.header.type = Attr::Type::AttributeList;
  list_attr.header.non_resident = 1;
  list_attr.header.name_length = 0;
  list_attr.header.flags = 0;
  list_attr.header.id = 0;
  list_attr.start_vcn = 0;
  list_attr.last_vcn = 0;
  list_attr.data_run_offset = static_cast<WORD>(sizeof(list_attr));
  list_attr.comp_unit_size = 0;
  list_attr.real_size = entry_size;
  list_attr.alloc_size = cluster_size;
  list_attr.ini_size = entry_size;

  const std::span<BYTE> list_run =
      std::span<BYTE>(record).subspan(offset + list_attr.data_run_offset);
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(list_run, 0) = run_header4_lcn_bytes;
  gsl::at(list_run, 1) = 1;
  const DWORD lcn = mft_list_lcn;
  std::memcpy(&gsl::at(list_run, 2), &lcn, sizeof(lcn));
  gsl::at(list_run, 2 + sizeof(lcn)) = 0x00;  // terminate the run list
  list_attr.header.total_size =
      AlignAttrSize(sizeof(list_attr) + 2 + sizeof(lcn) + 1);
  offset += list_attr.header.total_size;

  auto& data_attr =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
          &record.at(offset));
  data_attr.header.type = Attr::Type::Data;
  data_attr.header.non_resident = 1;
  data_attr.header.name_length = 0;
  data_attr.header.flags = 0;
  data_attr.header.id = 0;
  data_attr.start_vcn = 0;
  data_attr.last_vcn = 0;
  data_attr.data_run_offset = static_cast<WORD>(sizeof(data_attr));
  data_attr.comp_unit_size = 0;
  data_attr.real_size = fake_file_record_size;
  data_attr.alloc_size = data_attr.real_size;
  data_attr.ini_size = data_attr.real_size;
  data_attr.header.total_size =
      AlignAttrSize(sizeof(data_attr) + run_list_room);
  record.at(offset + sizeof(data_attr)) = 0x00;

  WriteEndOfAttributesMarker(record, offset + data_attr.header.total_size);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithNonResidentMftAttributeList() {
  std::vector<BYTE> image =
      BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  // The one entry the resident list held, now stored in the list's cluster.
  auto& entry = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::AttributeList*>(
      &image.at(static_cast<size_t>(mft_list_lcn) * cluster_size));
  entry.attr_type = Attr::Type::Data;
  entry.record_size = gsl::narrow<WORD>(AlignAttrSize(
      NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size));
  entry.name_length = 0;
  entry.name_offset = 0;
  entry.start_vcn = mft_data_split_target_idx;
  entry.base_ref.segment_number = mft_data_split_ext_idx;
  entry.base_ref.sequence_number = 0;
  entry.attr_id = 0;

  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftRecordWithNonResidentAttributeList());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataExtentChain() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  // Entry 1 names mft_chain_ext_a, reachable only through entry 2's extent.
  const std::array<std::pair<ULONGLONG, ULONGLONG>, 2> continuations{
      {{mft_chain_ext_a, mft_chain_ext_a_start_vcn},
       {mft_chain_ext_b, mft_chain_ext_b_start_vcn}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftRecordWithDataContinuations(continuations));

  // Ext B: naively reachable (below Mft::Idx::User). Its own extent
  // covers mft_chain_ext_a's real location.
  PutMftRecord(image, mft_addr, mft_chain_ext_b,
               MakeMftDataContinuationExtensionRecord(
                   mft_chain_ext_b_start_vcn, mft_chain_ext_b_lcn,
                   mft_chain_ext_b_clusters));

  // Ext A's real bytes, mapped through ext B's extent; naive slot stays zero.
  const size_t ext_a_offset = (static_cast<size_t>(mft_chain_ext_b_lcn) +
                               (mft_chain_ext_a - mft_chain_ext_b_start_vcn)) *
                              cluster_size;
  PutRecordAt(image, ext_a_offset,
              MakeMftDataContinuationExtensionRecord(mft_chain_ext_a_start_vcn,
                                                     mft_chain_target_lcn, 1));

  // The target file record, only reachable once both hops resolve.
  FakeRecord target_record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  WriteEndOfAttributesMarker(target_record, attr_offset_value);
  PutRecordAt(image, static_cast<size_t>(mft_chain_target_lcn) * cluster_size,
              target_record);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithUnresolvableMftDataExtent() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  // Entry 1 is permanently unresolvable; entry 2 resolves fine.
  const std::array<std::pair<ULONGLONG, ULONGLONG>, 2> continuations{
      {{mft_unresolvable_ext_idx, mft_unresolvable_start_vcn},
       {mft_unresolvable_good_ext_idx, mft_unresolvable_good_start_vcn}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftRecordWithDataContinuations(continuations));

  // The good entry: naively reachable, its extent covers the good record.
  PutMftRecord(image, mft_addr, mft_unresolvable_good_ext_idx,
               MakeMftDataContinuationExtensionRecord(
                   mft_unresolvable_good_start_vcn, mft_unresolvable_good_lcn,
                   mft_unresolvable_good_clusters));
  FakeRecord good_record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  WriteEndOfAttributesMarker(good_record, attr_offset_value);
  PutRecordAt(
      image,
      (static_cast<size_t>(mft_unresolvable_good_lcn) +
       (mft_unresolvable_good_record - mft_unresolvable_good_start_vcn)) *
          cluster_size,
      good_record);

  // mft_unresolvable_ext_idx's slot stays zero-filled and unmapped.

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataLastVcnOverflow() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  const std::array<std::pair<ULONGLONG, ULONGLONG>, 1> continuations{
      {{mft_last_vcn_overflow_target_idx, 0}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftRecordWithDataContinuations(
                   continuations, mft_last_vcn_overflow_last_vcn));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeMftRealSize() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();

  const size_t mft_offset = static_cast<size_t>(mft_lcn) * cluster_size;
  auto& attr =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
          &image.at(mft_offset + attr_offset_value));
  attr.real_size = huge_mft_real_size;
  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithMftDataSplitLink(FakeExtensionLink link) {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  const std::array<std::pair<ULONGLONG, ULONGLONG>, 1> continuations{
      {{mft_data_split_ext_idx, mft_data_split_target_idx}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftRecordWithDataContinuations(continuations, 0,
                                                  link.entry_sequence));
  PutMftRecord(image, mft_addr, mft_data_split_ext_idx,
               MakeMftDataContinuationExtensionRecord(mft_data_split_target_idx,
                                                      mft_data_split_lcn, 1,
                                                      link.record_sequence));

  FakeRecord target_record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  WriteEndOfAttributesMarker(target_record, attr_offset_value);
  PutRecordAt(image, static_cast<size_t>(mft_data_split_lcn) * cluster_size,
              target_record);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  const std::array<std::pair<ULONGLONG, ULONGLONG>, 2> continuations{
      {{mft_two_extents_ext_idx, mft_two_extents_first_vcn},
       {mft_two_extents_ext_idx, mft_two_extents_second_vcn}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(Mft::Idx::Mft),
               MakeMftRecordWithDataContinuations(continuations));

  const std::vector<FakeDataRun> first_runs{{mft_two_extents_first_lcn, 1}};
  const std::vector<FakeDataRun> second_runs{{mft_two_extents_second_lcn, 1}};
  FakeRecord ext = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  SetRecordLink(ext, 0, static_cast<ULONGLONG>(Mft::Idx::Mft));
  DWORD ext_offset = attr_offset_value;
  ext_offset += WriteNonResidentAttr(
      ext, ext_offset, Attr::Type::Data,
      {.comp_unit_size = 0,
       .real_size = fake_file_record_size,
       .runs = first_runs,
       .overrides = {.start_vcn = mft_two_extents_first_vcn}});
  ext_offset += WriteNonResidentAttr(
      ext, ext_offset, Attr::Type::Data,
      {.comp_unit_size = 0,
       .real_size = fake_file_record_size,
       .runs = second_runs,
       .overrides = {.start_vcn = mft_two_extents_second_vcn}});
  WriteEndOfAttributesMarker(ext, ext_offset);
  PutMftRecord(image, mft_addr, mft_two_extents_ext_idx, ext);

  FakeRecord target_record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  WriteEndOfAttributesMarker(target_record, attr_offset_value);
  PutRecordAt(image,
              static_cast<size_t>(mft_two_extents_first_lcn) * cluster_size,
              target_record);
  PutRecordAt(image,
              static_cast<size_t>(mft_two_extents_second_lcn) * cluster_size,
              target_record);

  return image;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
