#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/filename-flag.h"
#include "data/filename-namespace.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/standard-information.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Header fields of a ScratchRecord a test may forge. The defaults describe a
// well-formed record of a volume whose fixup array follows its 48-byte header.
struct ScratchHeader {
  // Offset of the update sequence array. 0x2A is the NTFS 3.0 short header.
  WORD offset_of_us = 40;
  // Words of the array, the update sequence number included.
  WORD size_of_us = 3;
  // Offset of the first attribute.
  WORD offset_of_attr = 56;
  Data::FileRecordFlag flags = Data::FileRecordFlag::InUse;
  WORD sequence = 1;
  ULONGLONG ref_to_base = 0;
  // The used size the header declares. The library MUST NOT rely on it.
  DWORD used_size = 0;
};

// A file record built attribute by attribute, for a test that needs a layout
// no other fixture has. The update sequence number and the words it replaces
// are zero, so the record verifies as long as no attribute writes to the last
// two bytes of a 512-byte block. Keep a record under 500 bytes.
class ScratchRecord {
 public:
  explicit ScratchRecord(const ScratchHeader& header = {})
      : cursor_(header.offset_of_attr) {
    EditFileRecordHeader(bytes_, [&](Data::FileRecordHeader& raw) {
      raw.magic = Data::FileRecordHeader::file_record_magic;
      raw.offset_of_us = header.offset_of_us;
      raw.size_of_us = header.size_of_us;
      raw.offset_of_attr = header.offset_of_attr;
      raw.flags = header.flags;
      raw.seq_no = header.sequence;
      raw.ref_to_base = header.ref_to_base;
      raw.real_size = header.used_size;
      raw.alloc_size = fake_file_record_size;
    });
  }

  // Appends a resident attribute of any type value, unnamed.
  ScratchRecord& Resident(Attr::Type type, std::span<const BYTE> body) {
    Data::HeaderResident header{};
    header.header.type = type;
    header.header.total_size = Aligned(sizeof(header) + body.size());
    header.attr_size = gsl::narrow<DWORD>(body.size());
    header.attr_offset = static_cast<WORD>(sizeof(header));
    Put(cursor_, header);
    if (!body.empty()) {
      std::memcpy(&bytes_.at(cursor_ + sizeof(header)), body.data(),
                  body.size());
    }
    cursor_ += header.header.total_size;
    return *this;
  }

  // Appends an unnamed non-resident attribute from VCN 0. run_list holds the
  // encoded runs, terminator included. clusters is the VCN count it maps.
  ScratchRecord& NonResident(Attr::Type type, std::span<const BYTE> run_list,
                             ULONGLONG clusters, ULONGLONG real_size) {
    Data::HeaderNonResident header{};
    header.header.type = type;
    header.header.non_resident = 1;
    header.header.total_size = Aligned(sizeof(header) + run_list.size());
    header.last_vcn = clusters == 0 ? 0 : clusters - 1;
    header.data_run_offset = static_cast<WORD>(sizeof(header));
    header.alloc_size = clusters * fake_cluster_size;
    header.real_size = real_size;
    header.ini_size = real_size;
    Put(cursor_, header);
    std::memcpy(&bytes_.at(cursor_ + sizeof(header)), run_list.data(),
                run_list.size());
    cursor_ += header.header.total_size;
    return *this;
  }

  // Appends the 4-byte marker that ends the attributes.
  ScratchRecord& End() {
    Put(cursor_, static_cast<DWORD>(Attr::Type::All));
    cursor_ += sizeof(DWORD);
    return *this;
  }

  // Appends bytes as they are, where the next attribute header would be.
  ScratchRecord& Raw(std::span<const BYTE> garbage) {
    std::memcpy(&bytes_.at(cursor_), garbage.data(), garbage.size());
    cursor_ += garbage.size();
    return *this;
  }

  // Byte offset the next attribute is written at.
  [[nodiscard]] std::size_t Cursor() const noexcept { return cursor_; }

  // Writes value at offset of the record.
  template <typename T>
  void Put(std::size_t offset, const T& value) {
    std::memcpy(&bytes_.at(offset), &value, sizeof(T));
  }

  [[nodiscard]] std::span<const BYTE> Bytes() const noexcept { return bytes_; }

  // The image of BuildFakeNtfsImage() with this record in $MFT slot idx. The
  // slot MUST be free in that image and below Mft::Idx::User.
  [[nodiscard]] std::vector<BYTE> ImageIn(ULONGLONG idx = scratch_slot) const {
    std::vector<BYTE> image = BuildFakeNtfsImage();
    Place(image, idx);
    return image;
  }

  // Copies this record over $MFT slot idx of an image whose $MFT starts at
  // LCN 1, as every fake image's does.
  void Place(std::vector<BYTE>& image, ULONGLONG idx) const {
    std::memcpy(&image.at(fake_cluster_size + idx * fake_file_record_size),
                bytes_.data(), bytes_.size());
  }

  // Free slot of the base image the tests put a record in: below
  // Mft::Idx::User, so it is read straight from disk.
  static constexpr ULONGLONG scratch_slot = 6;

 private:
  static DWORD Aligned(std::size_t size) {
    return gsl::narrow<DWORD>((size + 7U) & ~std::size_t{7U});
  }

  // The array holds the record on 8-byte boundaries, as a buffer read from
  // disk is, so the library may bind structs onto the attributes.
  struct alignas(8) Storage : std::array<BYTE, fake_file_record_size> {};

  Storage bytes_{};
  std::size_t cursor_;
};

// The body of a $STANDARD_INFORMATION with every time set.
inline std::array<BYTE, sizeof(Data::StandardInformation)>
    ScratchStdInfoBody() {
  Data::StandardInformation info{};
  info.create_time = 0x01D87F3C00000000ULL;
  info.alter_time = info.create_time;
  info.mft_time = info.create_time;
  info.read_time = info.create_time;
  info.permission = Data::StdInfoPermission::Archive;
  std::array<BYTE, sizeof(info)> body{};
  std::memcpy(body.data(), &info, sizeof(info));
  return body;
}

// The body of a $FILE_NAME for name under the parent reference. name_length
// is what the body declares, which a test may forge beyond the name. A negative
// value declares the real length.
inline std::vector<BYTE> ScratchFileNameBody(std::wstring_view name,
                                             ULONGLONG parent_ref,
                                             int name_length = -1) {
  // The names of the tests stay in the basic plane: one unit per character.
  std::vector<WORD> units;
  for (const wchar_t unit : name) {
    units.push_back(static_cast<WORD>(unit));
  }
  std::vector<BYTE> body(offsetof(Data::Filename, name) + (units.size() * 2));
  Data::Filename header{};
  header.parent_ref = parent_ref;
  header.flags = Data::FilenameFlag::Archive;
  header.name_length = gsl::narrow<BYTE>(
      name_length < 0 ? static_cast<int>(units.size()) : name_length);
  header.name_space = Data::FilenameNamespace::Win32;
  std::memcpy(body.data(), &header, offsetof(Data::Filename, name));
  std::memcpy(&body.at(offsetof(Data::Filename, name)), units.data(),
              units.size() * 2);
  return body;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
