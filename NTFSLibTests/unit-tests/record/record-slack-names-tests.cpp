#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/record/carved-name.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Record = NTFS_BROWSER_NAMESPACE::Record;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeSlackName;

namespace {

// The live file of the fixture, in the directory Docs, and the freed file.
constexpr ULONGLONG report_record = Tests::mft_tree_report_idx;
constexpr ULONGLONG docs_record = Tests::mft_tree_docs_idx;
constexpr ULONGLONG freed_record = Tests::mft_tree_deleted_file_idx;

// Bytes of slack the fake records keep room for, before their fixup words.
constexpr size_t slack_room = 280;

const VolumeOptions both_flags{.include_deleted = true, .recover_errors = true};

// A volume over image, whose reader counts every call.
template <Cache::Strategy S>
struct Opened {
  std::unique_ptr<NtfsVolume<S>> volume;
  // Owned by volume.
  Tests::CountingDiskReader* counter{nullptr};
};

template <Cache::Strategy S>
Opened<S> Open(std::vector<BYTE> image, const VolumeOptions& options) {
  auto counter = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(std::move(image)));
  Opened<S> opened;
  opened.counter = counter.get();
  opened.volume = std::make_unique<NtfsVolume<S>>(std::move(counter), options);
  REQUIRE(opened.volume->IsVolumeOK());
  return opened;
}

// The fixture with names written into the slack of a record.
std::vector<BYTE> ImageWith(ULONGLONG record,
                            const std::vector<FakeSlackName>& names,
                            size_t offset_from_marker = 8) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  for (const FakeSlackName& name : names) {
    const std::vector<BYTE> bytes = Tests::EncodeSlackName(name);
    Tests::PutRecordSlack(image, record, offset_from_marker, bytes);
    offset_from_marker += bytes.size();
  }
  return image;
}

FakeSlackName Old(std::wstring name, ULONGLONG parent = docs_record) {
  return FakeSlackName{.name = std::move(name),
                       .parent_record = parent,
                       .with_entry_header = true};
}

// What a file record gives for one record number, with the names it carved.
template <Cache::Strategy S>
std::vector<Record::CarvedName> CarveRecord(const NtfsVolume<S>& volume,
                                            ULONGLONG record) {
  Io::FileRecord<S> file_record(volume);
  REQUIRE(file_record.ParseFileRecord(record));
  REQUIRE(file_record.ParseAttrs());
  return file_record.CarveSlackNames();
}

template <Cache::Strategy S>
Mft::MftTree Scan(const NtfsVolume<S>& volume) {
  return Mft::MftTree(volume);
}

template <Cache::Strategy S>
void CheckNamesAreCarvedByTheScan() {
  const auto opened =
      Open<S>(ImageWith(report_record, {Old(L"older.txt"), Old(L"oldest.txt")}),
              both_flags);

  const Mft::MftTree tree = Scan(*opened.volume);

  const Mft::MftTree::Entry* report = tree.Find(report_record);
  REQUIRE(report != nullptr);
  REQUIRE(report->carved_names.size() == 2);
  CHECK(report->carved_names.at(0).name == L"older.txt");
  CHECK(report->carved_names.at(1).name == L"oldest.txt");
  CHECK(report->carved_names.at(0).parent_known);
  CHECK(report->carved_names.at(0).parent_record == docs_record);
  CHECK(report->carved_names.at(0).times_known);
  CHECK_FALSE(report->carved_names.at(0).partial);
  CHECK(tree.Stats().carved_names == 2);
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    if (entry.record != report_record) {
      CHECK(entry.carved_names.empty());
    }
  }
}

template <Cache::Strategy S>
void CheckLiveFieldsAreUntouched() {
  const std::vector<BYTE> plain = Tests::BuildFakeNtfsImageWithFreedFileTree();
  const auto without = Open<S>(plain, both_flags);
  const auto with =
      Open<S>(ImageWith(report_record, {Old(L"older.txt"), Old(L"oldest.txt")}),
              both_flags);

  const Mft::MftTree left = Scan(*without.volume);
  const Mft::MftTree right = Scan(*with.volume);

  REQUIRE(left.Entries().size() == right.Entries().size());
  for (size_t i = 0; i < left.Entries().size(); ++i) {
    const Mft::MftTree::Entry& a = left.Entries().at(i);
    const Mft::MftTree::Entry& b = right.Entries().at(i);
    INFO("record " << a.record);
    CHECK(a.record == b.record);
    CHECK(a.sequence == b.sequence);
    CHECK(a.in_use == b.in_use);
    CHECK(a.size == b.size);
    CHECK(a.allocated_size == b.allocated_size);
    CHECK(a.names.size() == b.names.size());
    for (size_t n = 0; n < a.names.size(); ++n) {
      CHECK(a.names.at(n).name == b.names.at(n).name);
      CHECK(a.names.at(n).parent_record == b.names.at(n).parent_record);
      CHECK(a.names.at(n).parent_valid == b.names.at(n).parent_valid);
    }
    CHECK(left.GetPath(a.record) == right.GetPath(b.record));
  }
  CHECK(left.Stats().in_use == right.Stats().in_use);
  CHECK(left.Stats().deleted == right.Stats().deleted);
  CHECK(left.Stats().unreachable == right.Stats().unreachable);
}

template <Cache::Strategy S>
void CheckLiveNamesAreNotRepeated() {
  // The slack holds a copy of each live name of the record, a name of the same
  // record under another parent, and a name that is new.
  const auto opened = Open<S>(
      ImageWith(report_record, {Old(L"report.txt"), Old(L"REPORT~1.TXT"),
                                Old(L"report.txt", 5), Old(L"fresh.txt")}),
      both_flags);

  const auto carved = CarveRecord(*opened.volume, report_record);

  REQUIRE(carved.size() == 2);
  CHECK(carved.at(0).name == L"report.txt");
  CHECK(carved.at(0).parent_record == 5);
  CHECK(carved.at(1).name == L"fresh.txt");
}

template <Cache::Strategy S>
void CheckAPartialCopyOfALiveNameIsNotRepeated() {
  // The copy lost its parent: the name alone matches the live one.
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  const std::vector<BYTE> bytes =
      Tests::EncodeSlackName({.name = L"old.tmp", .with_entry_header = false});
  // The structure starts on the last four bytes of the terminator.
  Tests::PutRecordSlack(image, freed_record, 4, bytes);
  const auto opened = Open<S>(std::move(image), both_flags);

  CHECK(CarveRecord(*opened.volume, freed_record).empty());

  // The same bytes with another name are a new candidate, partial.
  std::vector<BYTE> other = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutRecordSlack(other, freed_record, 4,
                        Tests::EncodeSlackName({.name = L"old.tm2"}));
  const auto changed = Open<S>(std::move(other), both_flags);
  const auto carved = CarveRecord(*changed.volume, freed_record);
  REQUIRE(carved.size() == 1);
  CHECK(carved.front().partial);
}

template <Cache::Strategy S>
void CheckNoFlagCombinationButBothCarves() {
  const std::vector<BYTE> image = ImageWith(report_record, {Old(L"older.txt")});
  for (const VolumeOptions& options :
       {VolumeOptions{}, VolumeOptions{.include_deleted = true},
        VolumeOptions{.recover_errors = true}}) {
    const auto opened = Open<S>(image, options);

    const Mft::MftTree tree = Scan(*opened.volume);

    CHECK(tree.Stats().carved_names == 0);
    for (const Mft::MftTree::Entry& entry : tree.Entries()) {
      CHECK(entry.carved_names.empty());
    }
    CHECK(CarveRecord(*opened.volume, report_record).empty());
  }
}

template <Cache::Strategy S>
void CheckSlackNeverCostsARead() {
  const std::vector<BYTE> plain = Tests::BuildFakeNtfsImageWithFreedFileTree();
  const std::vector<BYTE> slack =
      ImageWith(report_record, {Old(L"older.txt"), Old(L"oldest.txt")});
  for (const VolumeOptions& options :
       {VolumeOptions{}, VolumeOptions{.include_deleted = true},
        VolumeOptions{.recover_errors = true}, both_flags}) {
    const auto without = Open<S>(plain, options);
    const auto with = Open<S>(slack, options);
    without.counter->Reset();
    with.counter->Reset();

    const Mft::MftTree left = Scan(*without.volume);
    const Mft::MftTree right = Scan(*with.volume);

    INFO(options.include_deleted << options.recover_errors);
    REQUIRE(without.counter->ReadCount() == with.counter->ReadCount());
    for (size_t i = 0; i < without.counter->Calls().size(); ++i) {
      CHECK(without.counter->Calls().at(i).offset ==
            with.counter->Calls().at(i).offset);
      CHECK(without.counter->Calls().at(i).length ==
            with.counter->Calls().at(i).length);
    }
  }
}

}  // namespace

TEST_CASE("a scan carves the names left in the slack of a record",
          "[record][slack]") {
  CheckNamesAreCarvedByTheScan<Cache::Strategy::NoCache>();
  CheckNamesAreCarvedByTheScan<Cache::Strategy::FullCache>();
}

TEST_CASE("carving leaves every live field of the scan as it was",
          "[record][slack]") {
  CheckLiveFieldsAreUntouched<Cache::Strategy::NoCache>();
  CheckLiveFieldsAreUntouched<Cache::Strategy::FullCache>();
}

TEST_CASE("a carved name equal to a live name of the record is not repeated",
          "[record][slack]") {
  CheckLiveNamesAreNotRepeated<Cache::Strategy::NoCache>();
  CheckLiveNamesAreNotRepeated<Cache::Strategy::FullCache>();
}

TEST_CASE("a partial copy of a live name is not repeated", "[record][slack]") {
  CheckAPartialCopyOfALiveNameIsNotRepeated<Cache::Strategy::NoCache>();
  CheckAPartialCopyOfALiveNameIsNotRepeated<Cache::Strategy::FullCache>();
}

TEST_CASE("only recovery with deleted items shown carves", "[record][slack]") {
  CheckNoFlagCombinationButBothCarves<Cache::Strategy::NoCache>();
  CheckNoFlagCombinationButBothCarves<Cache::Strategy::FullCache>();
}

TEST_CASE("carving makes no read the scan would not make", "[record][slack]") {
  CheckSlackNeverCostsARead<Cache::Strategy::NoCache>();
  CheckSlackNeverCostsARead<Cache::Strategy::FullCache>();
}

TEST_CASE("both cache strategies carve the same names", "[record][slack]") {
  const std::vector<BYTE> image =
      ImageWith(report_record, {Old(L"older.txt"), Old(L"oldest.txt")});
  const auto no_cache = Open<Cache::Strategy::NoCache>(image, both_flags);
  const auto full_cache = Open<Cache::Strategy::FullCache>(image, both_flags);

  const auto left = CarveRecord(*no_cache.volume, report_record);
  const auto right = CarveRecord(*full_cache.volume, report_record);

  REQUIRE(left.size() == 2);
  REQUIRE(left.size() == right.size());
  for (size_t i = 0; i < left.size(); ++i) {
    CHECK(left.at(i).name == right.at(i).name);
    CHECK(left.at(i).parent_record == right.at(i).parent_record);
    CHECK(left.at(i).create_time.dwLowDateTime ==
          right.at(i).create_time.dwLowDateTime);
    CHECK(left.at(i).create_time.dwHighDateTime ==
          right.at(i).create_time.dwHighDateTime);
  }
}

TEST_CASE("carved times are converted like the other times of the scan",
          "[record][slack]") {
  const FakeSlackName slack = Old(L"older.txt");
  const auto opened = Open<Cache::Strategy::NoCache>(
      ImageWith(report_record, {slack}), both_flags);

  const auto carved = CarveRecord(*opened.volume, report_record);

  REQUIRE(carved.size() == 1);
  FILETIME expected{.dwLowDateTime = static_cast<DWORD>(slack.times.at(0)),
                    .dwHighDateTime =
                        static_cast<DWORD>(slack.times.at(0) >> 32U)};
#ifdef _WIN32
  FILETIME local{};
  if (FileTimeToLocalFileTime(&expected, &local) != 0) {
    expected = local;
  }
#endif
  CHECK(carved.front().create_time.dwLowDateTime == expected.dwLowDateTime);
  CHECK(carved.front().create_time.dwHighDateTime == expected.dwHighDateTime);
}

TEST_CASE("the slack of a freed record is carved", "[record][slack]") {
  const auto opened = Open<Cache::Strategy::NoCache>(
      ImageWith(freed_record, {Old(L"before-delete.txt")}), both_flags);

  const Mft::MftTree tree = Scan(*opened.volume);

  const Mft::MftTree::Entry* freed = tree.Find(freed_record);
  REQUIRE(freed != nullptr);
  CHECK_FALSE(freed->in_use);
  REQUIRE(freed->carved_names.size() == 1);
  CHECK(freed->carved_names.front().name == L"before-delete.txt");
}

TEST_CASE("a slack of zeros carves nothing", "[record][slack]") {
  const auto opened = Open<Cache::Strategy::NoCache>(
      Tests::BuildFakeNtfsImageWithFreedFileTree(), both_flags);

  const Mft::MftTree tree = Scan(*opened.volume);

  CHECK(tree.Stats().carved_names == 0);
}

TEST_CASE("a slack of random bytes carves nothing", "[record][slack]") {
  std::mt19937 engine(99);
  std::uniform_int_distribution<unsigned> byte(0, 255);
  for (int round = 0; round < 40; ++round) {
    std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
    std::vector<BYTE> noise(slack_room);
    for (BYTE& value : noise) {
      value = static_cast<BYTE>(byte(engine));
    }
    Tests::PutRecordSlack(image, docs_record, 8, noise);
    const auto opened =
        Open<Cache::Strategy::NoCache>(std::move(image), both_flags);

    INFO("round " << round);
    CHECK(CarveRecord(*opened.volume, docs_record).empty());
  }
}

TEST_CASE("slack up to the last byte of a record is carved",
          "[record][slack]") {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(Old(L"last.txt"));
  // The fake records keep a fixup array in their last six bytes: the name ends
  // right before it.
  const size_t marker = Tests::RecordEndMarkerOffset(image, report_record);
  constexpr size_t fixup_array = 1018;
  Tests::PutRecordSlack(image, report_record,
                        fixup_array - bytes.size() - marker, bytes);
  const auto opened =
      Open<Cache::Strategy::NoCache>(std::move(image), both_flags);

  const auto carved = CarveRecord(*opened.volume, report_record);

  REQUIRE(carved.size() == 1);
  CHECK(carved.front().name == L"last.txt");
}

TEST_CASE("a forged used size does not move the scan", "[record][slack]") {
  for (const DWORD used : {DWORD{0}, DWORD{16}, DWORD{0xFFFFFFFF}}) {
    std::vector<BYTE> image = ImageWith(report_record, {Old(L"older.txt")});
    Tests::SetRecordUsedSize(image, report_record, used);
    const auto opened =
        Open<Cache::Strategy::NoCache>(std::move(image), both_flags);

    INFO(used);
    const auto carved = CarveRecord(*opened.volume, report_record);
    REQUIRE(carved.size() == 1);
    CHECK(carved.front().name == L"older.txt");
  }
}

TEST_CASE("a record whose attributes never end carves nothing",
          "[record][slack]") {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  // An attribute that claims more bytes than the record holds replaces the end
  // marker: the walk runs out, and the names after it are not leftovers.
  std::vector<BYTE> broken(8);
  broken.at(0) = 0x10;
  broken.at(4) = 0xFF;
  broken.at(5) = 0x7F;
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(Old(L"older.txt"));
  Tests::PutRecordSlack(image, report_record, 8, bytes);
  Tests::PutRecordSlack(image, report_record, 0, broken);
  const auto opened =
      Open<Cache::Strategy::NoCache>(std::move(image), both_flags);

  Io::FileRecord<Cache::Strategy::NoCache> file_record(*opened.volume);
  REQUIRE(file_record.ParseFileRecord(report_record));
  static_cast<void>(file_record.ParseAttrs());

  CHECK(file_record.CarveSlackNames().empty());
}

TEST_CASE("nothing is carved before the attributes are parsed",
          "[record][slack]") {
  const auto opened = Open<Cache::Strategy::NoCache>(
      ImageWith(report_record, {Old(L"older.txt")}), both_flags);
  Io::FileRecord<Cache::Strategy::NoCache> file_record(*opened.volume);

  CHECK(file_record.CarveSlackNames().empty());
  REQUIRE(file_record.ParseFileRecord(report_record));
  CHECK(file_record.CarveSlackNames().empty());
  REQUIRE(file_record.ParseAttrs());
  CHECK(file_record.CarveSlackNames().size() == 1);
  // Another record replaces the slack.
  REQUIRE(file_record.ParseFileRecord(docs_record));
  CHECK(file_record.CarveSlackNames().empty());
}

TEST_CASE("the live getters answer the same before and after carving",
          "[record][slack]") {
  const auto opened = Open<Cache::Strategy::NoCache>(
      ImageWith(report_record, {Old(L"older.txt")}), both_flags);
  Io::FileRecord<Cache::Strategy::NoCache> file_record(*opened.volume);
  REQUIRE(file_record.ParseFileRecord(report_record));
  REQUIRE(file_record.ParseAttrs());
  const std::wstring name(file_record.GetFileName());
  const ULONGLONG size = file_record.GetFileSize();
  FILETIME write_before{};
  file_record.GetFileTime(&write_before, nullptr, nullptr);

  REQUIRE(file_record.CarveSlackNames().size() == 1);

  FILETIME write_after{};
  file_record.GetFileTime(&write_after, nullptr, nullptr);
  CHECK(std::wstring(file_record.GetFileName()) == name);
  CHECK(file_record.GetFileSize() == size);
  CHECK(write_after.dwLowDateTime == write_before.dwLowDateTime);
  CHECK(write_after.dwHighDateTime == write_before.dwHighDateTime);
}
