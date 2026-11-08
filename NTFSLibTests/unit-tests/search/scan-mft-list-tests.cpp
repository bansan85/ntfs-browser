#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "scan-support.h"
#include "search/engine.h"
#include "search/mft-list-engine.h"
#include "search/target.h"
#include "utf/utf.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
using TestSearch::Names;
using TestSearch::Record;
using TestSearch::Row;
using TestSearch::RunScan;
using TestSearch::TempImage;
using NameList = std::vector<std::string>;

namespace {

// Options for the MftList engine over target, listing the whole volume.
Lib::Options ListOptions(std::u16string target) {
  Lib::Options options;
  options.target = std::move(target);
  options.engine = Lib::EngineKind::MftList;
  return options;
}

// The row called name, or null. A hard link has one row for each name.
const Row* Find(const std::vector<Row>& rows, std::u16string_view name) {
  for (const Row& row : rows) {
    if (row.name == name) {
      return &row;
    }
  }
  return nullptr;
}

// The rows in the order of their record number, then of their name: the order
// that lets two engines be compared.
std::vector<Row> Sorted(std::vector<Row> rows) {
  std::ranges::sort(rows, [](const Row& left, const Row& right) {
    return left.record != right.record ? left.record < right.record
                                       : left.name < right.name;
  });
  return rows;
}

// What two engines MUST agree on for the same entry.
struct Identity {
  std::u16string name;
  std::u16string path;
  std::uint64_t record{0};
  std::uint16_t sequence{0};
  std::uint64_t parent_record{0};
  std::uint64_t size{0};
  bool is_directory{false};
  Lib::Standing standing{Lib::Standing::Live};
  Lib::DataState data_state{Lib::DataState::NotChecked};

  [[nodiscard]] bool operator==(const Identity&) const = default;
};

// The identities of rows. The sequence number and the size of a deleted entry
// are left out: a directory entry keeps those of the file when it was live,
// where the record holds the ones NTFS gave it when it freed it.
std::vector<Identity> IdentitiesOf(const std::vector<Row>& rows) {
  std::vector<Identity> identities;
  for (const Row& row : Sorted(rows)) {
    const bool live = row.standing == Lib::Standing::Live;
    identities.push_back({.name = row.name,
                          .path = row.path,
                          .record = row.record,
                          .sequence = live ? row.sequence : std::uint16_t{0},
                          .parent_record = row.parent_record,
                          .size = live ? row.size : 0,
                          .is_directory = row.is_directory,
                          .standing = row.standing,
                          .data_state = row.data_state});
  }
  return identities;
}

// An EntrySink that keeps the percents and the number of entries it was given.
class RecordingSink final : public Lib::EntrySink {
 public:
  void Add(const Lib::Entry& /*entry*/) override { ++entries; }

  void SetPercent(double percent) override { percents.push_back(percent); }

  std::size_t entries{0};
  std::vector<double> percents;
};

}  // namespace

TEST_CASE("The MftList engine lists the entries in record order",
          "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());

  const auto outcome = RunScan(ListOptions(image.Target()));

  REQUIRE(outcome.state == Lib::State::Finished);
  CHECK(Names(outcome.rows) ==
        NameList{"Docs", "a.txt", "c.txt", "Sub", "root.txt"});
  CHECK(outcome.progress.entries_found == 5);
}

TEST_CASE("The MftList engine reads the fields of the record",
          "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());
  const auto outcome = RunScan(ListOptions(image.Target()));
  REQUIRE(outcome.rows.size() == 5);

  const Row* file = Find(outcome.rows, u"a.txt");
  REQUIRE(file != nullptr);
  CHECK(file->record == Record(1));
  CHECK(file->parent_record == Record(0));
  CHECK(file->size == 10);
  CHECK(file->allocated_size == 16);
  CHECK(file->link_count == 1);
  CHECK(file->standing == Lib::Standing::Live);
  CHECK(file->data_state == Lib::DataState::NotChecked);
  CHECK_FALSE(file->has_clusters);
  CHECK_FALSE(file->is_directory);
  CHECK((file->attributes & Lib::Attribute::directory) == 0);
  CHECK(file->path.empty());

  CHECK(file->created == TestFake::std_info_create_time);
  CHECK(file->modified == TestFake::std_info_alter_time);
  CHECK(file->mft_changed == TestFake::std_info_mft_time);
  CHECK(file->accessed == TestFake::std_info_read_time);

  const Row* folder = Find(outcome.rows, u"Sub");
  REQUIRE(folder != nullptr);
  CHECK(folder->record == Record(3));
  CHECK(folder->is_directory);
  CHECK((folder->attributes & Lib::Attribute::directory) != 0);
  CHECK(folder->link_count == 1);
}

TEST_CASE("The MftList engine builds the full path of each entry",
          "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());
  Lib::Options options = ListOptions(image.Target());
  options.full_path = true;

  const auto outcome = RunScan(options);

  REQUIRE(outcome.rows.size() == 5);
  CHECK(Find(outcome.rows, u"Docs")->path == u"\\Docs");
  CHECK(Find(outcome.rows, u"root.txt")->path == u"\\root.txt");
  CHECK(Find(outcome.rows, u"a.txt")->path == u"\\Docs\\a.txt");
  CHECK(Find(outcome.rows, u"c.txt")->path == u"\\Docs\\Sub\\c.txt");
}

TEST_CASE("The MftList engine lists what the directory engines list",
          "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());
  const bool deleted = GENERATE(false, true);

  Lib::Options options = ListOptions(image.Target());
  options.include_deleted = deleted;
  options.full_path = true;
  const auto mft_list = RunScan(options);
  options.engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                            Lib::EngineKind::DirectoryFullCache);
  options.recursive = true;
  const auto directory = RunScan(options);

  REQUIRE(mft_list.state == Lib::State::Finished);
  REQUIRE(directory.state == Lib::State::Finished);
  CHECK(IdentitiesOf(mft_list.rows) == IdentitiesOf(directory.rows));
  CHECK(mft_list.rows.size() == (deleted ? 6U : 5U));
}

TEST_CASE("The MftList engine lists a deleted file when asked",
          "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());
  Lib::Options options = ListOptions(image.Target());

  SECTION("not by default") {
    CHECK(Find(RunScan(options).rows, u"gone.txt") == nullptr);
  }

  SECTION("with include_deleted") {
    options.include_deleted = true;

    const auto outcome = RunScan(options);

    const Row* gone = Find(outcome.rows, u"gone.txt");
    REQUIRE(gone != nullptr);
    CHECK(gone->record == Record(5));
    CHECK(gone->standing == Lib::Standing::Deleted);
    CHECK(gone->parent_record == Record(0));
    CHECK(Find(outcome.rows, u"a.txt")->standing == Lib::Standing::Live);
  }
}

TEST_CASE("The MftList engine hides the system files unless asked",
          "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());
  Lib::Options options = ListOptions(image.Target());
  const auto hidden = RunScan(options);
  options.include_system_files = true;
  const auto shown = RunScan(options);

  REQUIRE(hidden.state == Lib::State::Finished);
  CHECK(shown.rows.size() > hidden.rows.size());
  CHECK(Find(shown.rows, u"$MFT") != nullptr);
  for (const Row& row : hidden.rows) {
    CHECK(row.record >= 16);
  }
}

TEST_CASE("The MftList engine gives one entry for each hard link",
          "[search][mft-list]") {
  const TempImage image(TestSearch::HardLinkImage());
  Lib::Options options = ListOptions(image.Target());
  options.full_path = true;

  const auto outcome = RunScan(options);

  REQUIRE(outcome.state == Lib::State::Finished);
  CHECK(Names(outcome.rows) ==
        NameList{"Docs", "one.txt", "two.txt", "LongName.txt"});
  const Row* first = Find(outcome.rows, u"one.txt");
  const Row* second = Find(outcome.rows, u"two.txt");
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(first->record == Record(1));
  CHECK(second->record == Record(1));
  CHECK(first->link_count == 2);
  CHECK(second->link_count == 2);
  CHECK(first->path == u"\\one.txt");
  CHECK(second->path == u"\\Docs\\two.txt");
  CHECK(first->parent_record == 5);
  CHECK(second->parent_record == Record(0));
}

TEST_CASE("The MftList engine hides the DOS alias of a name",
          "[search][mft-list]") {
  const TempImage image(TestSearch::HardLinkImage());

  const auto outcome = RunScan(ListOptions(image.Target()));

  CHECK(Find(outcome.rows, u"LONGNA~1.TXT") == nullptr);
  const Row* file = Find(outcome.rows, u"LongName.txt");
  REQUIRE(file != nullptr);
  CHECK(file->link_count == 1);
}

TEST_CASE("The hard links are the names that the directory engines list",
          "[search][mft-list]") {
  const TempImage image(TestSearch::HardLinkImage());
  Lib::Options options = ListOptions(image.Target());
  const auto mft_list = RunScan(options);
  options.engine = Lib::EngineKind::DirectoryFullCache;
  options.recursive = true;
  const auto directory = RunScan(options);

  CHECK(Names(Sorted(mft_list.rows)) == Names(Sorted(directory.rows)));
}

TEST_CASE("The MftList engine lists an entry that no directory reaches",
          "[search][mft-list]") {
  const TempImage image(TestSearch::OrphanImage());
  Lib::Options options = ListOptions(image.Target());
  options.full_path = true;

  SECTION("under a placeholder when the parent is gone") {
    const auto outcome = RunScan(options);

    REQUIRE(outcome.state == Lib::State::Finished);
    CHECK(Names(outcome.rows) ==
          NameList{"Keep", "k.txt", "Mid", "orphan.txt"});
    CHECK(Find(outcome.rows, u"k.txt")->path == u"\\Keep\\k.txt");
    CHECK(Find(outcome.rows, u"Mid")->path == u"\\<lost>\\Mid");
    CHECK(Find(outcome.rows, u"orphan.txt")->path ==
          u"\\<lost>\\Mid\\orphan.txt");
    CHECK(Find(outcome.rows, u"orphan.txt")->parent_record == Record(3));
  }

  SECTION("under its own folders when the deleted parent is listed") {
    options.include_deleted = true;

    const auto outcome = RunScan(options);

    REQUIRE(outcome.state == Lib::State::Finished);
    CHECK(Find(outcome.rows, u"Gone")->path == u"\\Gone");
    CHECK(Find(outcome.rows, u"Mid")->path == u"\\Gone\\Mid");
    CHECK(Find(outcome.rows, u"orphan.txt")->path ==
          u"\\Gone\\Mid\\orphan.txt");
  }

  SECTION("that the directory engines cannot find") {
    options.engine = Lib::EngineKind::DirectoryFullCache;
    options.recursive = true;

    CHECK(Names(RunScan(options).rows) == NameList{"Keep", "k.txt"});
  }
}

TEST_CASE("The MftList engine counts the clusters of a deleted file",
          "[search][mft-list]") {
  const TempImage image(TestSearch::ReallocatedImage());
  Lib::Options options = ListOptions(image.Target());
  options.include_deleted = true;

  SECTION("by condition, for a deleted file only") {
    const auto outcome = RunScan(options);

    REQUIRE(outcome.state == Lib::State::Finished);
    const Row* live = Find(outcome.rows, u"live.bin");
    const Row* old = Find(outcome.rows, u"old.bin");
    const Row* free = Find(outcome.rows, u"free.bin");
    REQUIRE(live != nullptr);
    REQUIRE(old != nullptr);
    REQUIRE(free != nullptr);
    CHECK_FALSE(live->has_clusters);
    CHECK(old->has_clusters);
    CHECK(free->has_clusters);
    CHECK(old->data_state != Lib::DataState::NotChecked);
    CHECK(free->data_state != Lib::DataState::NotChecked);
    CHECK(old->standing == Lib::Standing::Deleted);
    CHECK(old->size == 2ULL * TestFake::fake_cluster_size);
  }

  SECTION("not when the data state is off") {
    options.deleted_data_state = false;

    const auto outcome = RunScan(options);

    const Row* old = Find(outcome.rows, u"old.bin");
    REQUIRE(old != nullptr);
    CHECK_FALSE(old->has_clusters);
    CHECK(old->data_state == Lib::DataState::NotChecked);
  }
}

TEST_CASE("The MftList engine reports a summary of the clusters",
          "[search][mft-list]") {
  const TempImage image(TestSearch::ReallocatedImage());
  Lib::Options options = ListOptions(image.Target());
  options.include_deleted = true;
  std::uint64_t reallocated = 0;
  std::uint64_t total = 0;
  Lib::Scan scan(options);
  scan.OnBatch([&](const Lib::Batch& batch) {
    for (const Lib::Entry& entry : batch.entries) {
      if (entry.name == u"old.bin" && entry.clusters != nullptr) {
        reallocated =
            entry.clusters->Count(Lib::ClusterCondition::ReallocatedWritten);
        for (const std::uint64_t count : entry.clusters->clusters) {
          total += count;
        }
      }
    }
  });
  scan.Start();
  static_cast<void>(TestSearch::PollToEnd(scan));

  CHECK(reallocated == 2);
  CHECK(total == 2);
}

TEST_CASE("The MftList engine filters by folder", "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());

  SECTION("the direct entries of a folder") {
    const auto outcome = RunScan(ListOptions(image.Target(u"Docs")));

    REQUIRE(outcome.state == Lib::State::Finished);
    CHECK(Names(outcome.rows) == NameList{"a.txt", "Sub"});
  }

  SECTION("everything below a folder when recursive") {
    Lib::Options options = ListOptions(image.Target(u"dOcS"));
    options.recursive = true;

    const auto outcome = RunScan(options);

    CHECK(Names(outcome.rows) == NameList{"a.txt", "c.txt", "Sub"});
  }

  SECTION("with the full path from the root of the volume") {
    Lib::Options options = ListOptions(image.Target(u"Docs/Sub"));
    options.full_path = true;

    const auto outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 1);
    CHECK(outcome.rows.front().path == u"\\Docs\\Sub\\c.txt");
  }

  SECTION("the whole volume, whatever recursive says, with no folder") {
    Lib::Options options = ListOptions(image.Target());
    options.recursive = false;

    CHECK(RunScan(options).rows.size() == 5);
  }
}

TEST_CASE("The MftList engine reports its progress", "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());

  SECTION("through a scan") {
    const auto outcome = RunScan(ListOptions(image.Target()));

    REQUIRE(outcome.progress.percent.has_value());
    CHECK(*outcome.progress.percent == 100.0);
  }

  SECTION("up to a hundred, never back") {
    const auto parts = Lib::SplitTarget(
        NTFS_BROWSER_NAMESPACE::Utf::Utf16ToWide(image.Target()));
    REQUIRE(parts.has_value());
    Lib::MftListEngine engine(ListOptions(image.Target()), *parts);
    RecordingSink sink;

    REQUIRE(engine.Run(sink, {}));

    REQUIRE_FALSE(sink.percents.empty());
    CHECK(sink.percents.back() == 100.0);
    CHECK(std::ranges::is_sorted(sink.percents));
    CHECK(sink.entries == 5);
  }
}

TEST_CASE("The MftList engine ends early when it is stopped",
          "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());
  const auto parts = Lib::SplitTarget(
      NTFS_BROWSER_NAMESPACE::Utf::Utf16ToWide(image.Target()));
  REQUIRE(parts.has_value());
  Lib::MftListEngine engine(ListOptions(image.Target()), *parts);
  RecordingSink sink;
  std::stop_source source;
  source.request_stop();

  CHECK(engine.Run(sink, source.get_token()));

  CHECK(sink.entries == 0);
}

TEST_CASE("Auto scans the root with MftList and a folder with a directory walk",
          "[search][mft-list]") {
  const TempImage image(TestSearch::TreeImage());

  SECTION("the choice") {
    Lib::Options options;
    Lib::TargetParts root;
    Lib::TargetParts folder;
    folder.inside = {L"Docs"};

    CHECK(Lib::ResolveEngine(options, root) == Lib::EngineKind::MftList);
    CHECK(Lib::ResolveEngine(options, folder) ==
          Lib::EngineKind::DirectoryFullCache);

    options.engine = Lib::EngineKind::DirectoryNoCache;
    CHECK(Lib::ResolveEngine(options, root) ==
          Lib::EngineKind::DirectoryNoCache);
    options.engine = Lib::EngineKind::MftList;
    CHECK(Lib::ResolveEngine(options, folder) == Lib::EngineKind::MftList);
  }

  SECTION("the root lists every entry") {
    Lib::Options options;
    options.target = image.Target();

    CHECK(Names(RunScan(options).rows) ==
          NameList{"Docs", "a.txt", "c.txt", "Sub", "root.txt"});
  }

  SECTION("a folder lists its entries in index order") {
    Lib::Options options;
    options.target = image.Target(u"Docs");

    CHECK(Names(RunScan(options).rows) == NameList{"a.txt", "Sub"});
  }
}

TEST_CASE("An MftList scan that cannot start ends Failed",
          "[search][mft-list]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TestSearch::TreeImage());

  SECTION("a file that is no volume") {
    const TempImage junk(std::vector<BYTE>(4096, 0));

    const auto outcome = RunScan(ListOptions(junk.Target()));

    CHECK(outcome.state == Lib::State::Failed);
    CHECK(outcome.rows.empty());
    CHECK(outcome.last_batches == 1);
  }

  SECTION("a folder that does not exist") {
    const auto outcome = RunScan(ListOptions(image.Target(u"Nope")));

    CHECK(outcome.state == Lib::State::Failed);
    CHECK(TestSupport::TakeCapturedLog().find("Cannot find directory Nope") !=
          std::string::npos);
  }

  SECTION("a file instead of a folder") {
    const auto outcome = RunScan(ListOptions(image.Target(u"Docs/a.txt")));

    CHECK(outcome.state == Lib::State::Failed);
  }

  static_cast<void>(TestSupport::TakeCapturedLog());
}
