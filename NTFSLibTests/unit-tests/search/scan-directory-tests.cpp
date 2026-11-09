#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "scan-support.h"
#include "win-types.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
using TestSearch::Names;
using TestSearch::Record;
using TestSearch::Row;
using TestSearch::RunScan;
using TestSearch::TempImage;
using TestSearch::TreeImage;
using NameList = std::vector<std::string>;

namespace {

// Options for a directory engine over target.
Lib::Options DirectoryOptions(std::u16string target, Lib::EngineKind engine) {
  Lib::Options options;
  options.target = std::move(target);
  options.engine = engine;
  return options;
}

// The row called name, or null.
const Row* Find(const std::vector<Row>& rows, std::u16string_view name) {
  for (const Row& row : rows) {
    if (row.name == name) {
      return &row;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("A folder scan lists the entries of one folder", "[search][scan]") {
  const TempImage image(TreeImage());
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);

  SECTION("the root") {
    const auto outcome = RunScan(DirectoryOptions(image.Target(), engine));

    CHECK(outcome.state == Lib::State::Finished);
    CHECK(Names(outcome.rows) == NameList{"Docs", "root.txt"});
  }

  SECTION("a folder inside the volume") {
    const auto outcome =
        RunScan(DirectoryOptions(image.Target(u"Docs"), engine));

    REQUIRE(outcome.state == Lib::State::Finished);
    CHECK(Names(outcome.rows) == NameList{"a.txt", "Sub"});
  }

  SECTION("a folder two levels down") {
    const auto outcome =
        RunScan(DirectoryOptions(image.Target(u"Docs/Sub"), engine));

    CHECK(Names(outcome.rows) == NameList{"c.txt"});
  }

  SECTION("a folder named in another case") {
    const auto outcome =
        RunScan(DirectoryOptions(image.Target(u"dOcS"), engine));

    CHECK(Names(outcome.rows) == NameList{"a.txt", "Sub"});
  }
}

TEST_CASE("A folder scan reports every field of an entry", "[search][scan]") {
  const TempImage image(TreeImage());
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);
  const auto outcome = RunScan(DirectoryOptions(image.Target(u"Docs"), engine));
  REQUIRE(outcome.rows.size() == 2);

  const Row* file = Find(outcome.rows, u"a.txt");
  REQUIRE(file != nullptr);
  CHECK(file->record == Record(1));
  CHECK(file->parent_record == Record(0));
  CHECK(file->size == 10);
  CHECK(file->allocated_size == 4096);
  CHECK_FALSE(file->is_directory);
  CHECK((file->attributes & Lib::Attribute::directory) == 0);
  CHECK(file->standing == Lib::Standing::Live);
  CHECK(file->data_state == Lib::DataState::NotChecked);
  CHECK(file->link_count == 0);
  CHECK_FALSE(file->has_clusters);
  CHECK(file->path.empty());
  CHECK(file->modified == TestFake::name_tree_write_time);

  const Row* folder = Find(outcome.rows, u"Sub");
  REQUIRE(folder != nullptr);
  CHECK(folder->record == Record(3));
  CHECK(folder->is_directory);
  CHECK((folder->attributes & Lib::Attribute::directory) != 0);
}

TEST_CASE("A recursive scan walks the tree depth first", "[search][scan]") {
  const TempImage image(TreeImage());
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = DirectoryOptions(image.Target(), engine);
  options.recursive = true;

  SECTION("from the root") {
    const auto outcome = RunScan(options);

    CHECK(outcome.state == Lib::State::Finished);
    CHECK(Names(outcome.rows) ==
          NameList{"Docs", "root.txt", "a.txt", "Sub", "c.txt"});
    CHECK(outcome.progress.entries_found == 5);
    CHECK_FALSE(outcome.progress.percent.has_value());
  }

  SECTION("from a folder") {
    options.target = image.Target(u"Docs");

    CHECK(Names(RunScan(options).rows) == NameList{"a.txt", "Sub", "c.txt"});
  }
}

TEST_CASE("A scan builds the full path of each entry", "[search][scan]") {
  const TempImage image(TreeImage());
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = DirectoryOptions(image.Target(), engine);
  options.recursive = true;
  options.full_path = true;

  SECTION("from the root") {
    const auto outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 5);
    CHECK(Find(outcome.rows, u"root.txt")->path == u"\\root.txt");
    CHECK(Find(outcome.rows, u"a.txt")->path == u"\\Docs\\a.txt");
    CHECK(Find(outcome.rows, u"c.txt")->path == u"\\Docs\\Sub\\c.txt");
  }

  SECTION("from a folder, with the name the volume holds") {
    options.target = image.Target(u"dOcS");

    const auto outcome = RunScan(options);

    REQUIRE(outcome.rows.size() == 3);
    CHECK(Find(outcome.rows, u"a.txt")->path == u"\\Docs\\a.txt");
    CHECK(Find(outcome.rows, u"c.txt")->path == u"\\Docs\\Sub\\c.txt");
  }
}

TEST_CASE("A scan lists the deleted files only when asked", "[search][scan]") {
  const TempImage image(TreeImage());
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = DirectoryOptions(image.Target(u"Docs"), engine);

  SECTION("not by default") {
    CHECK(Names(RunScan(options).rows) == NameList{"a.txt", "Sub"});
  }

  SECTION("with include_deleted") {
    options.include_deleted = true;

    const auto outcome = RunScan(options);

    CHECK(Names(outcome.rows) == NameList{"a.txt", "gone.txt", "Sub"});
    const Row* gone = Find(outcome.rows, u"gone.txt");
    REQUIRE(gone != nullptr);
    CHECK(gone->record == Record(5));
    CHECK((gone->standing == Lib::Standing::Deleted ||
           gone->standing == Lib::Standing::Reused));
    CHECK(Find(outcome.rows, u"a.txt")->standing == Lib::Standing::Live);
  }

  SECTION("with a data state for the deleted file") {
    options.include_deleted = true;

    const auto outcome = RunScan(options);

    const Row* gone = Find(outcome.rows, u"gone.txt");
    REQUIRE(gone != nullptr);
    CHECK(gone->standing == Lib::Standing::Deleted);
    CHECK(gone->data_state == Lib::DataState::Intact);
  }

  SECTION("without one when the option is off") {
    options.include_deleted = true;
    options.deleted_data_state = false;

    const auto outcome = RunScan(options);

    const Row* gone = Find(outcome.rows, u"gone.txt");
    REQUIRE(gone != nullptr);
    CHECK(gone->data_state == Lib::DataState::NotChecked);
  }
}

TEST_CASE("The two directory engines give the same entries", "[search][scan]") {
  const TempImage image(TreeImage());
  const bool recursive = GENERATE(false, true);
  const bool deleted = GENERATE(false, true);
  const bool full_path = GENERATE(false, true);
  const std::u16string inside = GENERATE(as<std::u16string>{}, u"", u"Docs");

  Lib::Options options =
      DirectoryOptions(image.Target(inside), Lib::EngineKind::DirectoryNoCache);
  options.recursive = recursive;
  options.include_deleted = deleted;
  options.full_path = full_path;
  const auto no_cache = RunScan(options);
  options.engine = Lib::EngineKind::DirectoryFullCache;
  const auto full_cache = RunScan(options);
  options.engine = Lib::EngineKind::Auto;
  const auto automatic = RunScan(options);

  REQUIRE(no_cache.state == Lib::State::Finished);
  CHECK_FALSE(no_cache.rows.empty());
  CHECK(no_cache.rows == full_cache.rows);
  if (!inside.empty()) {
    CHECK(automatic.rows == full_cache.rows);
  }
}

TEST_CASE("A scan keeps a name of unusual UTF-16 as it is", "[search][scan]") {
  const std::wstring unpaired{static_cast<wchar_t>(0xD800), L'a'};
  const std::wstring astral = L"b\U0001F600";
  std::vector<TestFake::FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = unpaired}}});
  nodes.push_back({.links = {{.name = astral}}});
  const TempImage image(TestFake::BuildFakeNtfsImageWithNameTree(nodes));
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);

  const auto outcome = RunScan(DirectoryOptions(image.Target(), engine));

  REQUIRE(outcome.rows.size() == 2);
  CHECK(Find(outcome.rows,
             std::u16string{static_cast<char16_t>(0xD800), u'a'}) != nullptr);
  CHECK(Find(outcome.rows, std::u16string{u'b', static_cast<char16_t>(0xD83D),
                                          static_cast<char16_t>(0xDE00)}) !=
        nullptr);
}

TEST_CASE("A scan hides the system files unless asked", "[search][scan]") {
  const TempImage image(TreeImage());
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options = DirectoryOptions(image.Target(), engine);
  const auto hidden = RunScan(options);
  options.include_system_files = true;
  const auto shown = RunScan(options);

  REQUIRE(hidden.state == Lib::State::Finished);
  CHECK(shown.rows.size() >= hidden.rows.size());
  for (const Row& row : hidden.rows) {
    CHECK(row.record >= 16);
  }
}

TEST_CASE("A scan that cannot start ends Failed", "[search][scan]") {
  TestSupport::InstallCaptureSink();
  const TempImage image(TreeImage());

  SECTION("a target that names nothing") {
    const auto outcome =
        RunScan(DirectoryOptions(u"/no/such/image.img", Lib::EngineKind::Auto));

    CHECK(outcome.state == Lib::State::Failed);
    CHECK(outcome.rows.empty());
    CHECK(outcome.last_batches == 1);
  }

  SECTION("a folder that does not exist") {
    const auto outcome =
        RunScan(DirectoryOptions(image.Target(u"Nope"), Lib::EngineKind::Auto));

    CHECK(outcome.state == Lib::State::Failed);
    CHECK(outcome.last_batches == 1);
    CHECK(TestSupport::TakeCapturedLog().find("Cannot find directory Nope") !=
          std::string::npos);
  }

  SECTION("a file instead of a folder") {
    const auto outcome = RunScan(
        DirectoryOptions(image.Target(u"Docs/a.txt"), Lib::EngineKind::Auto));

    CHECK(outcome.state == Lib::State::Failed);
  }

  SECTION("a file that is no volume") {
    const TempImage junk(std::vector<BYTE>(4096, 0));

    const auto outcome =
        RunScan(DirectoryOptions(junk.Target(), Lib::EngineKind::Auto));

    CHECK(outcome.state == Lib::State::Failed);
  }

  static_cast<void>(TestSupport::TakeCapturedLog());
}
