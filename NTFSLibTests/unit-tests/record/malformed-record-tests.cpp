#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "data/file-record-flag.h"
#include "memory-disk-reader.h"
#include "scratch-record.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Data = NTFS_BROWSER_NAMESPACE::Data;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::ScratchFileNameBody;
using NTFS_BROWSER_NAMESPACE::Tests::ScratchHeader;
using NTFS_BROWSER_NAMESPACE::Tests::ScratchRecord;
using NTFS_BROWSER_NAMESPACE::Tests::ScratchStdInfoBody;

namespace {

// Reference of the root directory, at the sequence number 5 a real root has.
constexpr ULONGLONG root_reference = 5ULL | (5ULL << 48U);

// Bytes of the unnamed $DATA the scratch records carry.
constexpr std::array<BYTE, 3> file_content{'a', 'b', 'c'};

// Body of an attribute of a type this library does not read.
constexpr std::array<BYTE, 8> filler{1, 2, 3, 4, 5, 6, 7, 8};

// A name length no 1-name $FILE_NAME of this size can hold: the largest a
// $FILE_NAME declares in its length byte.
constexpr int huge_name_length = 255;

// Offset of the used size in a file record header.
constexpr std::size_t used_size_offset = 24;

// Offset of the NTFS 3.0 short header's update sequence array: it begins two
// bytes after the 40-byte header, so the 4-byte record number is not there.
constexpr WORD short_header_us_offset = 0x2A;

// Offset of the first attribute of that short header.
constexpr WORD short_header_attr_offset = 0x30;

// Offsets of the two words the array replaces at the ends of the 512-byte
// blocks. In a short header they sit where the record number would be.
constexpr std::size_t first_replaced_word = 0x2C;
constexpr std::size_t second_replaced_word = 0x2E;

// Values of those words, none of them zero.
constexpr WORD first_replacement = 0xABCD;
constexpr WORD second_replacement = 0x1234;

// A record, the volume that holds it and its parse.
template <Cache::Strategy S>
struct Parsed {
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<FileRecord<S>> record;
  bool attrs_ok{false};
};

template <Cache::Strategy S>
Parsed<S> ParseScratch(const ScratchRecord& scratch,
                       const VolumeOptions& options) {
  Parsed<S> parsed;
  parsed.volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<MemoryDiskReader>(scratch.ImageIn()), options);
  REQUIRE(parsed.volume->IsVolumeOK());
  parsed.record = std::make_unique<FileRecord<S>>(*parsed.volume);
  REQUIRE(parsed.record->ParseFileRecord(ScratchRecord::scratch_slot));
  parsed.attrs_ok = parsed.record->ParseAttrs();
  return parsed;
}

// How many attributes of type the record holds.
template <Cache::Strategy S>
std::size_t Count(const Parsed<S>& parsed, Attr::Type type) {
  return parsed.record->GetAttr(type).size();
}

// The bytes of the record's unnamed $DATA.
template <Cache::Strategy S>
std::vector<BYTE> DataBytes(const Parsed<S>& parsed) {
  const auto& streams = parsed.record->GetAttr(Attr::Type::Data);
  REQUIRE(streams.size() == 1);
  std::vector<BYTE> bytes(streams.front()->GetDataSize());
  REQUIRE(streams.front()->ReadData(0, bytes) == bytes.size());
  return bytes;
}

VolumeOptions Options(bool recover, bool include_deleted = false) {
  return VolumeOptions{.include_deleted = include_deleted,
                       .recover_errors = recover};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "An attribute of an unknown type is skipped and its neighbours are kept",
    "[malformed-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto std_info = ScratchStdInfoBody();
  ScratchRecord scratch;
  scratch
      .Resident(Attr::Type::StandardInformation, std_info)
      // 0x5000 is past every real type, 0x1234 has a low nibble, 0x110 is just
      // past the last real type, and 0 is no type at all.
      .Resident(static_cast<Attr::Type>(0x5000), filler)
      .Resident(static_cast<Attr::Type>(0x1234), filler)
      .Resident(static_cast<Attr::Type>(0x110), filler)
      .Resident(static_cast<Attr::Type>(0), filler)
      .Resident(Attr::Type::Data, file_content)
      .End();

  for (const bool recover : {false, true}) {
    CAPTURE(recover);
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(recover));
    CHECK(parsed.attrs_ok);
    CHECK(Count(parsed, Attr::Type::StandardInformation) == 1);
    CHECK(Count(parsed, Attr::Type::Data) == 1);
    CHECK(DataBytes(parsed) ==
          std::vector<BYTE>(file_content.begin(), file_content.end()));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A freed record whose used size stops before its attributes is read to "
    "its end marker",
    "[malformed-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  // The header alone, a size inside the first attribute, nothing, and a size
  // past the whole record.
  const DWORD used_size = GENERATE(56U, 100U, 0U, 0xFFFFFFFFU);
  CAPTURE(used_size);
  const auto std_info = ScratchStdInfoBody();
  const auto name = ScratchFileNameBody(L"gone.txt", root_reference);
  ScratchRecord scratch(ScratchHeader{.flags = Data::FileRecordFlag::None,
                                      .used_size = used_size});
  scratch.Resident(Attr::Type::StandardInformation, std_info)
      .Resident(Attr::Type::FileName, name)
      .Resident(Attr::Type::Data, file_content)
      .End();

  for (const bool recover : {false, true}) {
    CAPTURE(recover);
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(recover, true));
    CHECK(parsed.record->IsDeleted());
    CHECK(parsed.attrs_ok);
    CHECK(Count(parsed, Attr::Type::StandardInformation) == 1);
    CHECK(Count(parsed, Attr::Type::FileName) == 1);
    CHECK(DataBytes(parsed) ==
          std::vector<BYTE>(file_content.begin(), file_content.end()));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "Garbage where the end marker should be rejects the record, or keeps the "
    "attributes before it",
    "[malformed-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto std_info = ScratchStdInfoBody();
  const auto name = ScratchFileNameBody(L"gone.txt", root_reference);
  ScratchRecord scratch(ScratchHeader{.flags = Data::FileRecordFlag::None});
  scratch.Resident(Attr::Type::StandardInformation, std_info)
      .Resident(Attr::Type::FileName, name);
  // The used size ends with the last attribute, and zeros follow it.
  scratch.Put<DWORD>(used_size_offset, gsl::narrow<DWORD>(scratch.Cursor()));
  scratch.Raw(std::array<BYTE, 16>{});

  SECTION("strict") {
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(false, true));
    CHECK_FALSE(parsed.attrs_ok);
    CHECK(Count(parsed, Attr::Type::StandardInformation) == 0);
    CHECK(Count(parsed, Attr::Type::FileName) == 0);
  }

  SECTION("recovering") {
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(true, true));
    CHECK_FALSE(parsed.attrs_ok);
    CHECK(Count(parsed, Attr::Type::StandardInformation) == 1);
    CHECK(Count(parsed, Attr::Type::FileName) == 1);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A $FILE_NAME whose name length overruns its value rejects the record, or "
    "keeps the attributes before it",
    "[malformed-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto std_info = ScratchStdInfoBody();
  const auto name = ScratchFileNameBody(L"x", root_reference, huge_name_length);
  ScratchRecord scratch;
  scratch.Resident(Attr::Type::StandardInformation, std_info)
      .Resident(Attr::Type::FileName, name)
      .Resident(Attr::Type::Data, file_content)
      .End();

  SECTION("strict") {
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(false));
    CHECK_FALSE(parsed.attrs_ok);
    CHECK(Count(parsed, Attr::Type::StandardInformation) == 0);
    CHECK(Count(parsed, Attr::Type::FileName) == 0);
  }

  SECTION("recovering") {
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(true));
    CHECK_FALSE(parsed.attrs_ok);
    CHECK(Count(parsed, Attr::Type::StandardInformation) == 1);
    CHECK(Count(parsed, Attr::Type::FileName) == 0);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A $FILE_NAME that fails to parse does not hide the attributes after it",
    "[malformed-record][!shouldfail]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto std_info = ScratchStdInfoBody();
  const auto broken =
      ScratchFileNameBody(L"x", root_reference, huge_name_length);
  const auto sound = ScratchFileNameBody(L"ok.txt", root_reference);
  ScratchRecord scratch;
  scratch.Resident(Attr::Type::StandardInformation, std_info)
      .Resident(Attr::Type::FileName, broken)
      .Resident(Attr::Type::FileName, sound)
      .Resident(Attr::Type::Data, file_content)
      .End();

  SECTION("strict") {
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(false));
    CHECK_FALSE(parsed.attrs_ok);
    CHECK(Count(parsed, Attr::Type::StandardInformation) == 0);
    CHECK(Count(parsed, Attr::Type::FileName) == 0);
    CHECK(Count(parsed, Attr::Type::Data) == 0);
  }

  SECTION("recovering") {
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(true));
    // An attribute was lost: the result is partial, as it always was.
    CHECK_FALSE(parsed.attrs_ok);
    CHECK(Count(parsed, Attr::Type::StandardInformation) == 1);
    CHECK(Count(parsed, Attr::Type::FileName) == 1);
    CHECK(DataBytes(parsed) ==
          std::vector<BYTE>(file_content.begin(), file_content.end()));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "An attribute of an impossible length still ends the walk",
    "[malformed-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto std_info = ScratchStdInfoBody();
  ScratchRecord scratch;
  scratch.Resident(Attr::Type::StandardInformation, std_info);
  // A $FILE_NAME header that declares 4 bytes: it says where nothing starts.
  std::array<BYTE, 16> header{};
  const DWORD type = static_cast<DWORD>(Attr::Type::FileName);
  const DWORD total_size = 4;
  std::memcpy(header.data(), &type, sizeof(type));
  std::memcpy(&header.at(sizeof(type)), &total_size, sizeof(total_size));
  scratch.Raw(header).Resident(Attr::Type::Data, file_content).End();

  for (const bool recover : {false, true}) {
    CAPTURE(recover);
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(recover));
    CHECK_FALSE(parsed.attrs_ok);
    CHECK(Count(parsed, Attr::Type::StandardInformation) ==
          (recover ? 1U : 0U));
    CHECK(Count(parsed, Attr::Type::Data) == 0);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A record with the NTFS 3.0 short header keeps its attributes",
    "[malformed-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto std_info = ScratchStdInfoBody();
  const auto name = ScratchFileNameBody(L"short.txt", root_reference);
  ScratchRecord scratch(
      ScratchHeader{.offset_of_us = short_header_us_offset,
                    .size_of_us = 3,
                    .offset_of_attr = short_header_attr_offset});
  // The array holds the words of the block ends, where a later header keeps
  // the record number.
  scratch.Put<WORD>(first_replaced_word, first_replacement);
  scratch.Put<WORD>(second_replaced_word, second_replacement);
  scratch.Resident(Attr::Type::StandardInformation, std_info)
      .Resident(Attr::Type::FileName, name)
      .Resident(Attr::Type::Data, file_content)
      .End();

  for (const bool recover : {false, true}) {
    CAPTURE(recover);
    const Parsed<S> parsed = ParseScratch<S>(scratch, Options(recover));
    CHECK(parsed.attrs_ok);
    CHECK(parsed.record->GetSequenceNumber() == 1);
    CHECK_FALSE(parsed.record->IsExtensionRecord());
    CHECK(Count(parsed, Attr::Type::StandardInformation) == 1);
    CHECK(Count(parsed, Attr::Type::FileName) == 1);
    CHECK(DataBytes(parsed) ==
          std::vector<BYTE>(file_content.begin(), file_content.end()));
  }
}
