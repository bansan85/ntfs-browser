#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/example/ntfsundel/ntfsundel-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>
#include <ntfs-browser/tool/device-access/local-time.h>
#include <ntfs-browser/types.h>

#include "win-types.h"

namespace ExampleNtfsUndel = NTFS_BROWSER_NAMESPACE::Example::NtfsUndel;

namespace {

namespace Lib = NTFS_BROWSER_NAMESPACE;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
using ExampleNtfsUndel::DeletedFile;
using ExampleNtfsUndel::SearchControl;
using ExampleNtfsUndel::SearchResult;
using NTFS_BROWSER_NAMESPACE::Test::Io::TempImage;
using References = std::vector<std::uint64_t>;

// The target text of an image, with a folder inside it when inside is not
// empty.
std::wstring Target(const TempImage& image, std::wstring_view inside = {}) {
  std::wstring target = image.path.generic_wstring();
  if (!inside.empty()) {
    target += L'/';
    target += inside;
  }
  return target;
}

// Lists the deleted files of the image, which must be a volume.
SearchResult List(const TempImage& image, const SearchControl& control = {},
                  std::wstring_view inside = {}) {
  std::optional<SearchResult> result = ExampleNtfsUndel::ListDeletedFiles(
      Target(image, inside), /*recover_errors=*/false, control);
  REQUIRE(result.has_value());
  return *std::move(result);
}

// The record numbers of the files a search listed, in order.
References Records(const SearchResult& result) {
  References references;
  for (const DeletedFile& file : result.files) {
    references.push_back(file.reference);
  }
  return references;
}

// The record numbers of the files whose name matches wildcard.
References Named(const TempImage& image, std::wstring_view wildcard) {
  const auto found =
      ExampleNtfsUndel::FindNamedFiles(Target(image), false, wildcard);
  REQUIRE(found.has_value());
  return References(found->begin(), found->end());
}

// $STANDARD_INFORMATION alter time the fake image writes into every record.
constexpr std::uint64_t fake_write_time = 0x1112131415161718ULL;

}  // namespace

TEST_CASE("NtfsUndel search lists freed records in record number order",
          "[undel][search][characterization]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = List(image);

  CHECK(Records(result) == References{19, 20, 21, 22});
  REQUIRE(result.files.size() == 4);
  CHECK(result.files[0].name == L"old.tmp");
  CHECK(result.files[1].name == L"OldDir");
  CHECK(result.files[2].name == L"draft.doc");
  CHECK(result.files[3].name == L"stale.txt");
  CHECK_FALSE(result.truncated);
}

TEST_CASE("NtfsUndel search builds each path from the directory entries",
          "[undel][search][characterization]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = List(image);

  REQUIRE(result.files.size() == 4);
  CHECK(result.files[0].full_path == L"\\Docs\\old.tmp");
  CHECK(result.files[1].full_path == L"\\Docs\\OldDir");
  // draft.doc names OldDir as its parent, and no directory reaches stale.txt.
  CHECK(result.files[2].full_path == L"\\Docs\\OldDir\\draft.doc");
  CHECK(result.files[3].full_path == L"\\<lost>\\stale.txt");
}

TEST_CASE("NtfsUndel search paths start at the root when it is named",
          "[undel][search][characterization]") {
  for (const std::wstring_view root_name : {L".", L"Volume"}) {
    const TempImage image(
        TestFakeImage::BuildFakeNtfsImageWithNamedRoot(root_name));

    const SearchResult result = List(image);

    REQUIRE(result.files.size() == 4);
    CHECK(result.files[0].full_path == L"\\Docs\\old.tmp");
    CHECK(result.files[2].full_path == L"\\draft.doc");
  }
}

TEST_CASE("NtfsUndel search ends on a loop of directory entries",
          "[undel][search]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithIndexCycle());

  const SearchResult result = List(image);

  CHECK_FALSE(result.files.empty());
}

TEST_CASE("NtfsUndel search reports the write time of each freed record",
          "[undel][search][characterization]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = List(image);

  REQUIRE(result.files.size() == 4);
  for (const DeletedFile& file : result.files) {
    CHECK(file.write_time == fake_write_time);
  }
}

TEST_CASE("NtfsUndel search gives each file a handle that names its record",
          "[undel][search]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = List(image);

  for (const DeletedFile& file : result.files) {
    CHECK(file.handle.record == file.reference);
    CHECK(file.handle.deleted);
  }
}

TEST_CASE("NtfsUndel search lists a record with several names once",
          "[undel][search]") {
  using TestFakeImage::FakeTreeNode;
  std::vector<FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"Docs"}}, .directory = true});
  nodes.push_back(
      {.links = {{.name = L"one.txt"}, {.name = L"two.txt", .parent = 0}},
       .in_use = false,
       .data_size = 3});
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithNameTree(nodes));

  const SearchResult result = List(image);

  CHECK(result.files.size() == 1);
}

TEST_CASE("NtfsUndel search restricts the list to a folder",
          "[undel][search]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = List(image, {}, L"Docs");

  CHECK(Records(result) == References{19, 20, 21});
}

TEST_CASE("NtfsUndel search takes a wildcard on the name",
          "[undel][search][characterization]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  CHECK(Named(image, L"old.tmp") == References{19});
  CHECK(Named(image, L"OLD*") == References{19, 20});
  CHECK(Named(image, L"*.doc") == References{21});
  CHECK(Named(image, L"?tale.txt") == References{22});
  CHECK(Named(image, L"old").empty());
}

TEST_CASE("NtfsUndel search matches a wildcard with a slash on the path",
          "[undel][search][characterization]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  CHECK(Named(image, L"Docs/*") == References{19, 20, 21});
  CHECK(Named(image, L"\\Docs\\old.*") == References{19});
  CHECK(Named(image, L"\\old.*").empty());
}

TEST_CASE("NtfsUndel search finds a non-ASCII name by its other case",
          "[undel][search][characterization]") {
  struct Names {
    std::wstring stored;
    std::wstring pattern;
  };

  const std::vector<Names> cases{{L"été.txt", L"ÉTÉ.*"},
                                 {L"ÉTÉ.txt", L"été.TXT"},
                                 {L"αβ.txt", L"ΑΒ.txt"},
                                 {L"Дом.txt", L"д?м.*"}};
  for (const Names& names : cases) {
    const TempImage image(
        TestFakeImage::BuildFakeNtfsImageWithFreedFileName(names.stored));

    CHECK(Named(image, names.pattern) == References{19});
  }
}

TEST_CASE("NtfsUndel search does not match another letter",
          "[undel][search][characterization]") {
  const TempImage image(
      TestFakeImage::BuildFakeNtfsImageWithFreedFileName(L"été.txt"));

  CHECK(Named(image, L"ete.txt").empty());
}

TEST_CASE("NtfsUndel search stops at once when stop is requested",
          "[undel][search][characterization]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const SearchControl control{.stop_requested = [] { return true; }};

  const std::optional<SearchResult> result =
      ExampleNtfsUndel::ListDeletedFiles(Target(image), false, control);
  CHECK(result.has_value());
}

TEST_CASE("NtfsUndel search truncates the list at max_listed",
          "[undel][search][characterization]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const SearchResult result = List(image, {.max_listed = 2});

  CHECK(Records(result) == References{19, 20});
  CHECK(result.truncated);
}

TEST_CASE("NtfsUndel search flags truncation when the list fills exactly",
          "[undel][search][characterization]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  // Four freed files fill a list of four. The flag still says the list was
  // cut.
  const SearchResult result = List(image, {.max_listed = 4});

  CHECK(result.files.size() == 4);
  CHECK(result.truncated);
}

TEST_CASE("NtfsUndel too-many-files message names the row limit",
          "[undel][search]") {
  CHECK(ExampleNtfsUndel::TooManyFilesMessage().find(std::to_wstring(
            ExampleNtfsUndel::kMaxListedFiles)) != std::wstring::npos);
}

TEST_CASE("NtfsUndel search lists kMaxListedFiles rows by default",
          "[undel][search]") {
  CHECK(SearchControl{}.max_listed == ExampleNtfsUndel::kMaxListedFiles);
}

TEST_CASE("NtfsUndel search cannot list a file that is no volume",
          "[undel][search]") {
  const TempImage image(std::vector<BYTE>(65536, 'x'));

  CHECK_FALSE(
      ExampleNtfsUndel::ListDeletedFiles(Target(image), false, {}).has_value());
}

TEST_CASE("NtfsUndel text goes to UTF-16 units as it is", "[undel][search]") {
  CHECK(ExampleNtfsUndel::ToUtf16(L"abc") == u"abc");
  CHECK(ExampleNtfsUndel::ToUtf16(L"\U0001D11E") == u"\U0001D11E");
  CHECK(ExampleNtfsUndel::ToUtf16(L"\xD83D") == u"\xD83D");
}

TEST_CASE("NtfsUndel local time prints the local date and time",
          "[undel][search][characterization]") {
  // 2000-01-01 12:00 UTC: the same day in every zone but the far ones.
  constexpr std::uint64_t noon =
      125'911'584'000'000'000ULL + 12ULL * 3600 * 10'000'000;
  const auto local =
      NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess::UtcTicksToLocalTime(noon);
  REQUIRE(local.has_value());

  const std::wstring text = ExampleNtfsUndel::FormatLocalTime(noon);

  CHECK(text.size() == 17);
  CHECK(text.substr(0, 4) == std::to_wstring(local->year));
}

TEST_CASE("NtfsUndel local time prints zeros for an unconvertible time",
          "[undel][search][characterization]") {
  CHECK(ExampleNtfsUndel::FormatLocalTime(~0ULL) == L"0000-00-00  00:00");
}
