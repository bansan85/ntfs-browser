#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "data/header-non-resident.h"
#include "data/index-block.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "fake-ntfs-image.h"
#include "lznt1/decompress.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;
namespace Data = NtfsBrowser::Data;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::VolumeOptions;

namespace {

// Fake images put $MFT at LCN 1, so record 0 starts one cluster into the image.
constexpr size_t fake_mft_offset = NtfsBrowserTests::fake_cluster_size;

// Fake images place the forged index block at this LCN.
constexpr size_t index_block_lcn = 20;

// Byte offset of the first attribute field in a file record header.
constexpr size_t record_first_attr_field = 0x14;

// Byte offsets of the common attribute header fields.
constexpr size_t attr_type_field = 0;
constexpr size_t attr_total_size_field = 4;

// Byte offset of a resident attribute's value offset field.
constexpr size_t resident_body_offset_field = 0x14;

// Byte offset of a resident attribute's value length field.
constexpr size_t resident_body_size_field = 0x10;

// Terminates the attribute list of a file record.
constexpr DWORD end_of_attributes = 0xFFFFFFFF;

// Index entry flag bit that marks the node's last entry.
constexpr BYTE last_entry_flag = static_cast<BYTE>(Data::IndexEntryFlag::Last);

// Update sequence value written into every sector of a test index block.
constexpr WORD usn_value = 0x0001;

// Index block layout: where its USN array sits, and how many words it holds.
constexpr WORD block_usn_offset = 40;
constexpr WORD block_usn_words = 15;

// Where a test index block puts its only entry, and that entry's size.
constexpr size_t block_entries_offset = 88;
constexpr WORD end_marker_size = 16;

// Bytes per update sequence stride; sector ends hold the USN.
constexpr size_t sector_stride = 512;

// Size of the test index block, the forged block's size in the fake image.
constexpr size_t test_block_size = 7 * 1024;

// Salvages a damaged entry list instead of rejecting it.
constexpr VolumeOptions recovering{.recover_errors = true};

template <Cache::Strategy S>
struct ParsedRecord {
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<FileRecord<S>> record;
};

// Serves an image like MemoryDiskReader, but fails or throws on any read
// that touches [fault_begin, fault_end).
class FaultyDiskReader final : public NtfsBrowser::IDiskReader {
 public:
  enum class Fault { Fail, Throw };

  FaultyDiskReader(std::vector<BYTE> image, ULONGLONG fault_begin,
                   ULONGLONG fault_end, Fault fault)
      : inner_(std::move(image)),
        fault_begin_(fault_begin),
        fault_end_(fault_end),
        fault_(fault) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  // Faults stay off until Arm(), so the volume can open and parse first.
  void Arm() { armed_ = true; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    const auto begin = static_cast<ULONGLONG>(addr.QuadPart);
    const ULONGLONG end = begin + dest.size();
    if (armed_ && begin < fault_end_ && fault_begin_ < end) {
      if (fault_ == Fault::Throw) {
        throw std::runtime_error("injected read fault");
      }
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

 private:
  NtfsBrowserTests::MemoryDiskReader inner_;
  ULONGLONG fault_begin_;
  ULONGLONG fault_end_;
  Fault fault_;
  bool armed_ = false;
};

// Fake images put their compressed or stored unit data at LCN 30 onwards.
constexpr ULONGLONG compressed_fault_begin =
    30ULL * NtfsBrowserTests::fake_cluster_size;
constexpr ULONGLONG compressed_fault_end =
    40ULL * NtfsBrowserTests::fake_cluster_size;

// Opens record idx through a volume over reader, not yet attribute-parsed.
template <Cache::Strategy S>
ParsedRecord<S> OpenRecordFrom(std::unique_ptr<NtfsBrowser::IDiskReader> reader,
                               ULONGLONG idx, VolumeOptions options = {}) {
  ParsedRecord<S> parsed;
  parsed.volume = std::make_unique<NtfsVolume<S>>(std::move(reader), options);
  REQUIRE(parsed.volume->IsVolumeOK());

  parsed.record = std::make_unique<FileRecord<S>>(*parsed.volume);
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

template <typename T>
T ReadAt(const std::vector<BYTE>& image, size_t offset) {
  T value{};
  std::memcpy(&value, &image.at(offset), sizeof(T));
  return value;
}

template <typename T>
void WriteAt(std::vector<BYTE>& image, size_t offset, T value) {
  std::memcpy(&image.at(offset), &value, sizeof(T));
}

// Byte offset of record idx within the image.
size_t RecordOffset(ULONGLONG idx) {
  return fake_mft_offset +
         static_cast<size_t>(NtfsBrowserTests::fake_file_record_size) *
             static_cast<size_t>(idx);
}

// Byte offset of the first attribute of the given type in record idx.
size_t FindAttr(const std::vector<BYTE>& image, ULONGLONG idx,
                Attr::Type type) {
  const size_t record = RecordOffset(idx);
  size_t attr = record + ReadAt<WORD>(image, record + record_first_attr_field);
  while (ReadAt<DWORD>(image, attr + attr_type_field) != end_of_attributes) {
    if (ReadAt<DWORD>(image, attr + attr_type_field) ==
        static_cast<DWORD>(type)) {
      return attr;
    }
    const auto length = ReadAt<DWORD>(image, attr + attr_total_size_field);
    REQUIRE(length != 0);
    attr += length;
  }
  FAIL("attribute missing from the record");
  return 0;
}

// Byte offset of a resident attribute's value.
size_t ResidentBody(const std::vector<BYTE>& image, size_t attr) {
  return attr + ReadAt<WORD>(image, attr + resident_body_offset_field);
}

// Byte offset of the $INDEX_ROOT value in record idx.
size_t IndexRootBody(const std::vector<BYTE>& image, ULONGLONG idx) {
  return ResidentBody(image, FindAttr(image, idx, Attr::Type::IndexRoot));
}

// Byte offset of the first entry of an $INDEX_ROOT value.
size_t RootFirstEntry(const std::vector<BYTE>& image, size_t body) {
  constexpr size_t entry_offset_field = offsetof(Data::IndexRoot, entry_offset);
  return body + entry_offset_field +
         ReadAt<DWORD>(image, body + entry_offset_field);
}

// Byte offset of the $INDEX_ROOT entry that carries the Last flag.
size_t RootLastEntry(const std::vector<BYTE>& image, size_t body) {
  size_t entry = RootFirstEntry(image, body);
  while ((ReadAt<BYTE>(image, entry + offsetof(Data::IndexEntry, flags)) &
          last_entry_flag) == 0) {
    entry += ReadAt<WORD>(image, entry + offsetof(Data::IndexEntry, size));
  }
  return entry;
}

// Writes a block that passes FixupIndexBlock() and holds one end-marker entry.
void WriteValidIndexBlock(std::vector<BYTE>& image, size_t block) {
  WriteAt<DWORD>(image, block + offsetof(Data::IndexBlock, magic),
                 Data::index_block_magic);
  WriteAt<WORD>(image, block + offsetof(Data::IndexBlock, offset_of_us),
                block_usn_offset);
  WriteAt<WORD>(image, block + offsetof(Data::IndexBlock, size_of_us),
                block_usn_words);
  constexpr size_t entry_offset_field =
      offsetof(Data::IndexBlock, entry_offset);
  WriteAt<DWORD>(image, block + entry_offset_field,
                 static_cast<DWORD>(block_entries_offset - entry_offset_field));
  WriteAt<DWORD>(image, block + offsetof(Data::IndexBlock, total_entry_size),
                 end_marker_size);
  WriteAt<DWORD>(image, block + offsetof(Data::IndexBlock, alloc_entry_size),
                 end_marker_size);

  // The USN is stored in each sector's last word; the real words sit in the
  // array.
  WriteAt<WORD>(image, block + block_usn_offset, usn_value);
  for (size_t i = 0; i + 1 < block_usn_words; i++) {
    WriteAt<WORD>(image, block + block_usn_offset + sizeof(WORD) * (i + 1), 0);
    WriteAt<WORD>(image, block + (i + 1) * sector_stride - sizeof(WORD),
                  usn_value);
  }

  const size_t entry = block + block_entries_offset;
  WriteAt<WORD>(image, entry + offsetof(Data::IndexEntry, size),
                end_marker_size);
  WriteAt<WORD>(image, entry + offsetof(Data::IndexEntry, stream_size), 0);
  WriteAt<BYTE>(image, entry + offsetof(Data::IndexEntry, flags),
                last_entry_flag);
}

// Forged-index-block image with a valid block behind the directory's sub-node
// entry.
std::vector<BYTE> ImageWithValidIndexBlock() {
  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock();
  const size_t block = index_block_lcn * NtfsBrowserTests::fake_cluster_size;
  WriteValidIndexBlock(image, block);
  return image;
}

// Byte offset of the sub-node VCN of the directory's root entry.
size_t RootSubNodeVcnSlot(const std::vector<BYTE>& image, ULONGLONG idx) {
  const size_t entry = RootFirstEntry(image, IndexRootBody(image, idx));
  return entry + ReadAt<WORD>(image, entry + offsetof(Data::IndexEntry, size)) -
         sizeof(ULONGLONG);
}

// Byte offset of the index block in a forged-index-block image.
size_t IndexBlockOffset() {
  return index_block_lcn * NtfsBrowserTests::fake_cluster_size;
}

// Byte offset of the non-resident $INDEX_ALLOCATION of the forged directory.
size_t ForgedAllocAttr(const std::vector<BYTE>& image) {
  return FindAttr(image, NtfsBrowserTests::index_alloc_dir_idx,
                  Attr::Type::IndexAllocation);
}

// Byte offset of the first data run of a non-resident attribute.
size_t FirstRunOffset(const std::vector<BYTE>& image, size_t attr) {
  return attr + ReadAt<WORD>(image, attr + offsetof(Data::HeaderNonResident,
                                                    data_run_offset));
}

// Strict parsing rejects the record, while recovering parsing keeps going.
template <Cache::Strategy S>
void CheckStrictRejectsRecoveringAccepts(const std::vector<BYTE>& image,
                                         ULONGLONG idx) {
  auto strict = OpenRecord<S>(image, idx);
  CHECK_FALSE(strict.record->ParseAttrs());

  auto salvaging = OpenRecord<S>(image, idx, recovering);
  CHECK(salvaging.record->ParseAttrs());
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A malformed $INDEX_ROOT entry list is rejected or salvaged", "[cov-nri]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const ULONGLONG idx = NtfsBrowserTests::index_root_variant_a_dir_idx;

  SECTION("entry_offset points past the attribute") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants();
    WriteAt<DWORD>(image,
                   IndexRootBody(image, idx) +
                       offsetof(Data::IndexRoot, entry_offset),
                   0xFFFFFFF0U);
    CheckStrictRejectsRecoveringAccepts<S>(image, idx);
  }

  SECTION("an entry of size zero") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants();
    WriteAt<WORD>(image,
                  RootFirstEntry(image, IndexRootBody(image, idx)) +
                      offsetof(Data::IndexEntry, size),
                  0);
    CheckStrictRejectsRecoveringAccepts<S>(image, idx);
  }

  SECTION("an entry larger than the rest of the attribute") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants();
    WriteAt<WORD>(image,
                  RootFirstEntry(image, IndexRootBody(image, idx)) +
                      offsetof(Data::IndexEntry, size),
                  0xFFFF);
    CheckStrictRejectsRecoveringAccepts<S>(image, idx);
  }

  SECTION("entries that overrun the declared total entry size") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants();
    WriteAt<DWORD>(image,
                   IndexRootBody(image, idx) +
                       offsetof(Data::IndexRoot, total_entry_size),
                   1);
    CheckStrictRejectsRecoveringAccepts<S>(image, idx);
  }

  SECTION("the last entry lacks its Last flag, so the list runs out") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants();
    const size_t last = RootLastEntry(image, IndexRootBody(image, idx));
    WriteAt<BYTE>(image, last + offsetof(Data::IndexEntry, flags), 0);
    CheckStrictRejectsRecoveringAccepts<S>(image, idx);
  }

  SECTION("a resident value shorter than an IndexRoot header") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants();
    WriteAt<DWORD>(image,
                   FindAttr(image, idx, Attr::Type::IndexRoot) +
                       resident_body_size_field,
                   4);
    CHECK_FALSE(OpenRecord<S>(image, idx).record->ParseAttrs());
    CHECK_FALSE(OpenRecord<S>(image, idx, recovering).record->ParseAttrs());
  }

  SECTION("a root that is not a FileName index is skipped") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants();
    WriteAt<DWORD>(
        image, IndexRootBody(image, idx) + offsetof(Data::IndexRoot, attr_type),
        static_cast<DWORD>(Attr::Type::StandardInformation));

    auto parsed = OpenRecord<S>(image, idx);
    CHECK(parsed.record->ParseAttrs());
    CHECK_FALSE(
        parsed.record->FindSubEntry(NtfsBrowserTests::index_root_variant_a_name)
            .has_value());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A malformed $INDEX_ALLOCATION block is rejected or salvaged", "[cov-nri]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const ULONGLONG idx = NtfsBrowserTests::index_alloc_dir_idx;
  const auto name = NtfsBrowserTests::index_root_variant_a_name;

  SECTION("a sub-node VCN that overflows the byte offset") {
    auto image = ImageWithValidIndexBlock();
    WriteAt<ULONGLONG>(image, RootSubNodeVcnSlot(image, idx), ~ULONGLONG{0});
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }

  SECTION("a sub-node VCN that is not on a block boundary") {
    auto image = ImageWithValidIndexBlock();
    WriteAt<ULONGLONG>(image, RootSubNodeVcnSlot(image, idx), 1);
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }

  SECTION("a block without the INDX magic") {
    auto image = ImageWithValidIndexBlock();
    WriteAt<DWORD>(image,
                   IndexBlockOffset() + offsetof(Data::IndexBlock, magic), 0);
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }

  SECTION("entry_offset points past the end of the block") {
    auto image = ImageWithValidIndexBlock();
    WriteAt<DWORD>(
        image, IndexBlockOffset() + offsetof(Data::IndexBlock, entry_offset),
        0xFFFFFF);
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }

  SECTION("the last entry lacks its Last flag and fills the block") {
    auto image = ImageWithValidIndexBlock();
    const size_t entry = IndexBlockOffset() + block_entries_offset;
    const auto fill = static_cast<WORD>(test_block_size - block_entries_offset);
    WriteAt<WORD>(image, entry + offsetof(Data::IndexEntry, size), fill);
    WriteAt<DWORD>(image,
                   IndexBlockOffset() +
                       offsetof(Data::IndexBlock, total_entry_size),
                   fill);
    WriteAt<BYTE>(image, entry + offsetof(Data::IndexEntry, flags), 0);
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }

  SECTION("entries that overrun the block's declared total entry size") {
    auto image = ImageWithValidIndexBlock();
    WriteAt<DWORD>(
        image,
        IndexBlockOffset() + offsetof(Data::IndexBlock, total_entry_size), 8);
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }

  SECTION("an entry whose stream does not fit its size") {
    auto image = ImageWithValidIndexBlock();
    const size_t entry = IndexBlockOffset() + block_entries_offset;
    WriteAt<WORD>(image, entry + offsetof(Data::IndexEntry, stream_size), 4);
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }

  SECTION("a sub-node entry too small to hold its VCN") {
    auto image = ImageWithValidIndexBlock();
    const size_t entry = IndexBlockOffset() + block_entries_offset;
    WriteAt<BYTE>(image, entry + offsetof(Data::IndexEntry, flags),
                  static_cast<BYTE>(Data::IndexEntryFlag::SubNode) |
                      last_entry_flag);
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }

  SECTION("a block whose run points past the end of the image") {
    auto image = ImageWithValidIndexBlock();
    WriteAt<DWORD>(image, FirstRunOffset(image, ForgedAllocAttr(image)) + 2,
                   0x00FFFFFF);
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }

  SECTION("an allocation whose size is not a whole number of blocks") {
    auto image = ImageWithValidIndexBlock();
    WriteAt<ULONGLONG>(image,
                       ForgedAllocAttr(image) +
                           offsetof(Data::HeaderNonResident, real_size),
                       test_block_size + NtfsBrowserTests::fake_cluster_size);
    auto parsed = OpenRecord<S>(image, idx);
    REQUIRE(parsed.record->ParseAttrs());
    CHECK_FALSE(parsed.record->FindSubEntry(name).has_value());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A malformed $INDEX_ALLOCATION data run is rejected or salvaged",
    "[cov-nri]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const ULONGLONG idx = NtfsBrowserTests::index_alloc_dir_idx;

  SECTION("a run header whose length field exceeds 8 bytes") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock();
    WriteAt<BYTE>(image, FirstRunOffset(image, ForgedAllocAttr(image)), 0x4F);
    CheckStrictRejectsRecoveringAccepts<S>(image, idx);
  }

  SECTION("a run whose bytes overrun the attribute") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock();
    WriteAt<BYTE>(image, FirstRunOffset(image, ForgedAllocAttr(image)), 0x84);
    CheckStrictRejectsRecoveringAccepts<S>(image, idx);
  }

  SECTION("a run whose LCN falls below zero") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock();
    const size_t run = FirstRunOffset(image, ForgedAllocAttr(image));
    // One-byte length (7 clusters) and one-byte negative LCN delta of -128.
    WriteAt<BYTE>(image, run, 0x11);
    WriteAt<BYTE>(image, run + 1, 7);
    WriteAt<BYTE>(image, run + 2, 0x80);
    WriteAt<BYTE>(image, run + 3, 0);
    CheckStrictRejectsRecoveringAccepts<S>(image, idx);
  }

  SECTION("a run that extends past the attribute's last VCN") {
    auto image = NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock();
    WriteAt<ULONGLONG>(image,
                       ForgedAllocAttr(image) +
                           offsetof(Data::HeaderNonResident, last_vcn),
                       0);
    CheckStrictRejectsRecoveringAccepts<S>(image, idx);
  }
}

TEST_CASE("An index entry whose stream cannot fit its size has no name",
          "[cov-nri]") {
  std::vector<BYTE> buffer(256);
  auto& entry = *reinterpret_cast<Data::IndexEntry*>(buffer.data());

  SECTION("a stream claimed by an entry with no room past its header") {
    entry.size = static_cast<WORD>(offsetof(Data::IndexEntry, stream));
    entry.stream_size = 4;
    const IndexEntryView view{entry};
    CHECK_FALSE(view.HasName());
  }

  SECTION("a stream too small to hold a file name header") {
    entry.size = static_cast<WORD>(offsetof(Data::IndexEntry, stream) + 4);
    entry.stream_size = 4;
    const IndexEntryView view{entry};
    CHECK_FALSE(view.HasName());
  }

  SECTION("an entry without a stream copies cleanly, with no name") {
    entry.size = static_cast<WORD>(offsetof(Data::IndexEntry, stream));
    entry.stream_size = 0;
    entry.flags = Data::IndexEntryFlag::SubNode;
    const IndexEntry copy{IndexEntryView(entry)};
    CHECK_FALSE(copy.HasName());
    CHECK_FALSE(copy.IsSubNodePtr());
  }
}

TEST_CASE("LZNT1 rejects compressed output that overflows its bounds",
          "[lznt1][cov-nri]") {
  SECTION("a back-reference longer than the destination's room") {
    // Literal 'A', then a back-reference of length 3 into a 2-byte buffer.
    const std::vector<BYTE> src{0x03, 0xB0, 0x02, 'A', 0x00, 0x00};
    std::vector<BYTE> dest(2);
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, dest),
                    std::runtime_error);
  }

  SECTION("a literal past the end of the destination") {
    // Two literals, 'A' and 'B', into a 1-byte buffer.
    const std::vector<BYTE> src{0x02, 0xB0, 0x00, 'A', 'B'};
    std::vector<BYTE> dest(1);
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, dest),
                    std::runtime_error);
  }

  SECTION("elements that decode past 4096 bytes in one chunk") {
    // A 4095-byte back-reference fills the chunk, then one more literal
    // arrives.
    const std::vector<BYTE> src{0x05, 0xB0, 0x02, 'A', 0xFC, 0x0F, 'B', 'C'};
    std::vector<BYTE> dest(2 * NtfsBrowser::Lznt1::chunk_size);
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, dest),
                    std::runtime_error);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A non-resident attribute reads nothing into an empty buffer and fails "
    "past its end",
    "[cov-nri]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  auto parsed =
      OpenRecord<S>(NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock(),
                    NtfsBrowserTests::index_alloc_dir_idx);
  REQUIRE(parsed.record->ParseAttrs());
  const auto& alloc = parsed.record->GetAttr(Attr::Type::IndexAllocation);
  REQUIRE(alloc.size() == 1);

  std::vector<BYTE> buffer(16);
  const std::span<BYTE> empty;
  CHECK(alloc[0]->ReadData(0, empty) == 0);
  // The forged block's real size is 7 KiB, so one byte past it fails.
  const ULONGLONG past_end = test_block_size + 1;
  CHECK_FALSE(alloc[0]->ReadData(past_end, buffer).has_value());
  CHECK(alloc[0]->GetData() != nullptr);
}

namespace {

// Reads 16 bytes at an unaligned offset of the forged block, which fails in the
// given way on its first cluster.
template <Cache::Strategy S>
void CheckUnalignedFirstClusterFails(FaultyDiskReader::Fault fault) {
  const ULONGLONG block_begin =
      index_block_lcn * NtfsBrowserTests::fake_cluster_size;
  const ULONGLONG block_end = block_begin + NtfsBrowserTests::fake_cluster_size;
  auto reader = std::make_unique<FaultyDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock(), block_begin,
      block_end, fault);
  FaultyDiskReader* const faulty = reader.get();
  auto parsed = OpenRecordFrom<S>(std::move(reader),
                                  NtfsBrowserTests::index_alloc_dir_idx);
  faulty->Arm();
  REQUIRE(parsed.record->ParseAttrs());
  const auto& alloc = parsed.record->GetAttr(Attr::Type::IndexAllocation);
  REQUIRE(alloc.size() == 1);

  std::vector<BYTE> buffer(16);
  CHECK_FALSE(alloc[0]->ReadData(100, buffer).has_value());
}

}  // namespace

TEST_CASE("A cluster read that fails or throws yields no data, not a crash",
          "[cov-nri]") {
  // A FullCache volume serves these clusters from its cache, so only NoCache
  // reaches the failing read.
  SECTION("the unaligned first cluster read fails") {
    CheckUnalignedFirstClusterFails<Cache::Strategy::NoCache>(
        FaultyDiskReader::Fault::Fail);
  }

  SECTION("the unaligned first cluster read throws") {
    CheckUnalignedFirstClusterFails<Cache::Strategy::NoCache>(
        FaultyDiskReader::Fault::Throw);
  }
}

TEST_CASE("A compression unit whose clusters cannot be read yields no data",
          "[cov-nri]") {
  // A FullCache volume serves these clusters from its cache, so only NoCache
  // reaches the failing read.
  // Root directory record: it carries the compressed $DATA stream.
  constexpr ULONGLONG root_record_idx = 5;

  const auto check_unit_read_fails = [](std::vector<BYTE> image) {
    auto reader = std::make_unique<FaultyDiskReader>(
        std::move(image), compressed_fault_begin, compressed_fault_end,
        FaultyDiskReader::Fault::Fail);
    FaultyDiskReader* const faulty = reader.get();
    auto parsed = OpenRecordFrom<Cache::Strategy::NoCache>(std::move(reader),
                                                           root_record_idx);
    faulty->Arm();
    REQUIRE(parsed.record->ParseAttrs());
    const auto& data = parsed.record->GetAttr(Attr::Type::Data);
    REQUIRE(data.size() == 1);

    std::vector<BYTE> buffer(NtfsBrowserTests::compression_unit_size);
    CHECK_FALSE(data[0]->ReadData(0, buffer).has_value());
  };

  SECTION("a compressed unit whose real clusters cannot be read") {
    check_unit_read_fails(
        NtfsBrowserTests::BuildFakeNtfsImageWithCompressedFile());
  }

  SECTION("a stored unit whose clusters cannot be read") {
    check_unit_read_fails(
        NtfsBrowserTests::BuildFakeNtfsImageWithStoredCompressionUnit());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A read past the last mapped cluster fails instead of reading on",
    "[cov-nri]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("a raw data run") {
    std::vector<BYTE> image =
        NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock();
    const size_t attr = ForgedAllocAttr(image);
    constexpr ULONGLONG declared_size = 8192;
    WriteAt<ULONGLONG>(image,
                       attr + offsetof(Data::HeaderNonResident, real_size),
                       declared_size);
    WriteAt<ULONGLONG>(image,
                       attr + offsetof(Data::HeaderNonResident, ini_size),
                       declared_size);

    auto parsed =
        OpenRecord<S>(std::move(image), NtfsBrowserTests::index_alloc_dir_idx);
    REQUIRE(parsed.record->ParseAttrs());
    const auto& alloc = parsed.record->GetAttr(Attr::Type::IndexAllocation);
    REQUIRE(alloc.size() == 1);

    std::vector<BYTE> buffer(16);
    CHECK_FALSE(alloc[0]->ReadData(7168, buffer).has_value());
  }

  SECTION("a compressed unit") {
    std::vector<BYTE> image =
        NtfsBrowserTests::BuildFakeNtfsImageWithCompressedFile();
    constexpr ULONGLONG root_record_idx = 5;
    const size_t attr = FindAttr(image, root_record_idx, Attr::Type::Data);
    const auto last_vcn = ReadAt<ULONGLONG>(
        image, attr + offsetof(Data::HeaderNonResident, last_vcn));
    const ULONGLONG declared_size =
        (last_vcn + 2) * NtfsBrowserTests::fake_cluster_size;
    WriteAt<ULONGLONG>(image,
                       attr + offsetof(Data::HeaderNonResident, real_size),
                       declared_size);
    WriteAt<ULONGLONG>(image,
                       attr + offsetof(Data::HeaderNonResident, ini_size),
                       declared_size);

    auto parsed = OpenRecord<S>(std::move(image), root_record_idx);
    REQUIRE(parsed.record->ParseAttrs());
    const auto& data = parsed.record->GetAttr(Attr::Type::Data);
    REQUIRE(data.size() == 1);

    std::vector<BYTE> buffer(NtfsBrowserTests::fake_cluster_size);
    CHECK_FALSE(
        data[0]
            ->ReadData((last_vcn + 1) * NtfsBrowserTests::fake_cluster_size,
                       buffer)
            .has_value());
  }
}
