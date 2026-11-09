#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/example/ntfsundel/ntfsundel-cli.h>
#include <ntfs-browser/example/ntfsundel/ntfsundel-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/device-access/temp-dir.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>
#include <ntfs-browser/test/support/sample-run.h>
#include <ntfs-browser/tool/device-access/text-convert.h>

#include "sample-dump-support.h"
#include "win-types.h"

namespace TestIo = NTFS_BROWSER_NAMESPACE::Test::Io;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestAlloc = NTFS_BROWSER_NAMESPACE::Test::Alloc;
namespace ExampleNtfsUndel = NTFS_BROWSER_NAMESPACE::Example::NtfsUndel;
namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

namespace Fs = std::filesystem;
using ExampleNtfsUndel::Arguments;
using ExampleNtfsUndel::DeletedFile;
using ExampleNtfsUndel::Environment;
using NTFS_BROWSER_NAMESPACE::Test::DeviceAccess::TempDir;
using NTFS_BROWSER_NAMESPACE::Test::Io::TempImage;

namespace {

// The record, and the clusters, of the freed file that was reallocated.
constexpr std::uint64_t reallocated_ref = 22;
constexpr ULONGLONG reallocated_lcn = 33;

// Parses the arguments that follow the program name. error is set when the
// command line is refused.
std::optional<Arguments> Parse(std::initializer_list<std::wstring_view> args,
                               std::wstring& error) {
  TestIo::Argv argv(args);
  return ExampleNtfsUndel::ParseArguments(argv.Count(), argv.Data(), error);
}

// The arguments that follow the program name, for a command line that must
// be accepted.
Arguments MustParse(std::initializer_list<std::wstring_view> args) {
  std::wstring error;
  std::optional<Arguments> parsed = Parse(args, error);
  INFO(ToolDeviceAccess::WideToUtf8(error));
  REQUIRE(parsed.has_value());
  return *parsed;
}

// The message of a command line that must be refused.
std::wstring ParseError(std::initializer_list<std::wstring_view> args) {
  std::wstring error;
  CHECK_FALSE(Parse(args, error).has_value());
  return error;
}

// A freed file with only what the selection reads.
DeletedFile Freed(std::uint64_t reference, std::wstring name,
                  std::wstring full_path) {
  DeletedFile file;
  file.reference = reference;
  file.name = std::move(name);
  file.full_path = std::move(full_path);
  return file;
}

// The files of a small listing.
std::vector<DeletedFile> SampleFiles() {
  return {Freed(19, L"old.tmp", L"\\Docs\\old.tmp"),
          Freed(20, L"OldDir", L"\\Docs\\OldDir"),
          Freed(21, L"Draft.DOC", L"\\Draft.DOC"),
          Freed(22, L"stale.txt", L"stale.txt")};
}

// A volume identity that names the image and any other path apart: the image
// is on "source", every other path on "elsewhere". With same_drive, all on
// "source".
ExampleNtfsUndel::VolumeIdFunction Identity(const Fs::path& image,
                                            bool same_drive = false) {
  return
      [image, same_drive](const Fs::path& path) -> std::optional<std::string> {
        return path == image || same_drive ? "source" : "elsewhere";
      };
}

// What a run printed and returned.
using Run = TestIo::Captured;

// Runs the command over the image file at image with the given arguments.
// input is what the user types. A terminal is on input when interactive.
Run RunUndel(const Fs::path& image,
             std::initializer_list<std::wstring_view> args,
             const ExampleNtfsUndel::VolumeIdFunction& volume_id,
             const std::string& input = {}, bool interactive = false) {
  std::wstring error;
  std::vector<std::wstring> storage{L"tool", image.wstring()};
  for (const std::wstring_view arg : args) {
    storage.emplace_back(arg);
  }
  std::vector<wchar_t*> pointers;
  for (std::wstring& text : storage) {
    pointers.push_back(text.data());
  }
  const std::optional<Arguments> parsed = ExampleNtfsUndel::ParseArguments(
      static_cast<int>(pointers.size()), pointers.data(), error);
  INFO(ToolDeviceAccess::WideToUtf8(error));
  REQUIRE(parsed.has_value());

  std::istringstream in(input);
  return TestIo::Capture([&](std::FILE* out, std::FILE* err) {
    const Environment environment{.out = out,
                                  .err = err,
                                  .in = &in,
                                  .interactive = interactive,
                                  .volume_id = volume_id};
    return ExampleNtfsUndel::UndelTarget(*parsed, environment);
  });
}

// The bytes of the file at path, or nothing when it does not exist.
std::optional<std::string> Content(const Fs::path& path) {
  if (!Fs::exists(path)) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
}

// The freed tree with the freed "stale.txt" (22) on two clusters, one of which
// the bitmap gives to another file.
std::vector<BYTE> ReallocatedImage() {
  const std::uint64_t size = 2 * TestFakeImage::fake_cluster_size;
  std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithFreedNonResidentFile(
          size, {{.lcn = reallocated_lcn, .clusters = 2}});
  for (std::size_t i = 0; i < size; i++) {
    image.at(reallocated_lcn * TestFakeImage::fake_cluster_size + i) =
        static_cast<BYTE>(i % 251);
  }
  const std::vector<TestFakeImage::FakeDataRun> other_file{
      {.lcn = reallocated_lcn + 1, .clusters = 1}};
  return TestAlloc::WithBitmap(std::move(image), other_file);
}

}  // namespace

TEST_CASE("NtfsUndel arguments take the target and the options",
          "[ntfsundel][cli][arguments]") {
  const Arguments args =
      MustParse({L"--record", L"7", L"--record=9", L"--name", L"*.doc",
                 L"--name=a?", L"--output", L"out", L"--include-doubtful",
                 L"--all", L"--recover", L"/dev/sda1/Docs"});

  CHECK(args.target == L"/dev/sda1/Docs");
  CHECK(args.records == std::vector<std::uint64_t>{7, 9});
  CHECK(args.names == std::vector<std::wstring>{L"*.doc", L"a?"});
  CHECK(args.output == L"out");
  CHECK(args.include_doubtful);
  CHECK(args.all);
  CHECK(args.common.volume_options.recover_errors);
  CHECK(args.HasSelection());
}

TEST_CASE("NtfsUndel arguments default to a listing",
          "[ntfsundel][cli][arguments]") {
  const Arguments args = MustParse({L"c:"});

  CHECK(args.output.empty());
  CHECK_FALSE(args.HasSelection());
  CHECK_FALSE(args.include_doubtful);
}

TEST_CASE("NtfsUndel arguments read -o and an output after the target",
          "[ntfsundel][cli][arguments]") {
  const Arguments args = MustParse({L"disk.img", L"-o", L"out"});

  CHECK(args.target == L"disk.img");
  CHECK(args.output == L"out");
}

TEST_CASE("NtfsUndel --help needs no target", "[ntfsundel][cli][arguments]") {
  CHECK(MustParse({L"--help"}).common.help);
  CHECK(MustParse({L"-h"}).common.help);
}

TEST_CASE("NtfsUndel arguments are refused when they cannot run",
          "[ntfsundel][cli][arguments]") {
  CHECK(ParseError({}) == L"A target is needed");
  CHECK(ParseError({L"--all"}) == L"A target is needed");
  CHECK(ParseError({L"a", L"b"}) == L"Only one target is allowed");
  CHECK(ParseError({L"a", L"--bogus"}) == L"Unknown option --bogus");
  CHECK(ParseError({L"a", L"--record"}) == L"--record needs a record number");
  CHECK(ParseError({L"a", L"--record", L"x1"}) ==
        L"--record needs a record number");
  CHECK(ParseError({L"a", L"--record="}) == L"--record needs a record number");
  CHECK(ParseError({L"a", L"--name"}) == L"--name needs a pattern");
  CHECK(ParseError({L"a", L"--output"}) == L"--output needs a folder");
  CHECK(ParseError({L"a", L"-o="}) == L"--output needs a folder");
  CHECK(ParseError({L"a", L"--log=nonsense"}).starts_with(L"Malformed option"));
}

TEST_CASE("NtfsUndel usage names every option", "[ntfsundel][cli][arguments]") {
  const std::string usage = ExampleNtfsUndel::UsageText();

  for (const char* option :
       {"--all", "--record", "--name", "--output", "--include-doubtful",
        "--recover", "--log=", "--help", "-o DIR"}) {
    INFO(option);
    CHECK(usage.find(option) != std::string::npos);
  }
}

TEST_CASE("NtfsUndel selects by --all, record and name",
          "[ntfsundel][cli][selection]") {
  const std::vector<DeletedFile> files = SampleFiles();
  using Positions = std::vector<std::size_t>;
  const std::set<std::uint64_t> none;

  SECTION("all") {
    CHECK(
        ExampleNtfsUndel::SelectFiles(files, MustParse({L"x", L"--all"}), none)
            .positions == Positions{0, 1, 2, 3});
  }

  SECTION("a record, and one the list does not hold") {
    const ExampleNtfsUndel::Selection selection = ExampleNtfsUndel::SelectFiles(
        files, MustParse({L"x", L"--record", L"21", L"--record", L"99"}), none);
    CHECK(selection.positions == Positions{2});
    CHECK(selection.missing_records == std::vector<std::uint64_t>{99});
  }

  SECTION("the records that the name patterns found") {
    CHECK(ExampleNtfsUndel::SelectFiles(
              files, MustParse({L"x", L"--name", L"*.doc"}), {21})
              .positions == Positions{2});
  }

  SECTION("any of several selectors, each file once") {
    CHECK(ExampleNtfsUndel::SelectFiles(
              files,
              MustParse({L"x", L"--name", L"old*", L"--record", L"19",
                         L"--record", L"22"}),
              {19, 20})
              .positions == Positions{0, 1, 3});
  }

  SECTION("nothing asked, nothing chosen") {
    CHECK(ExampleNtfsUndel::SelectFiles(files, MustParse({L"x"}), none)
              .positions.empty());
  }
}

TEST_CASE("NtfsUndel reads a list of numbers", "[ntfsundel][cli][selection]") {
  using ExampleNtfsUndel::ParseNumberList;
  using Positions = std::vector<std::size_t>;

  CHECK(ParseNumberList("1", 5) == Positions{0});
  CHECK(ParseNumberList("1,3 5", 5) == Positions{0, 2, 4});
  CHECK(ParseNumberList("2-4", 5) == Positions{1, 2, 3});
  CHECK(ParseNumberList(" 4 , 2-3,2 ", 5) == Positions{3, 1, 2});
  CHECK(ParseNumberList("all", 3) == Positions{0, 1, 2});
  CHECK(ParseNumberList("", 3) == Positions{});
  CHECK(ParseNumberList("   ", 3) == Positions{});
  CHECK(ParseNumberList("3-3", 3) == Positions{2});
  CHECK_FALSE(ParseNumberList("0", 5));
  CHECK_FALSE(ParseNumberList("6", 5));
  CHECK_FALSE(ParseNumberList("x", 5));
  CHECK_FALSE(ParseNumberList("1-x", 5));
  CHECK_FALSE(ParseNumberList("4-2", 5));
  CHECK_FALSE(ParseNumberList("1-9", 5));
  CHECK_FALSE(ParseNumberList("-1", 5));
  CHECK_FALSE(ParseNumberList("1234567890123", 5));
  CHECK_FALSE(ParseNumberList("1", 0));
}

TEST_CASE("NtfsUndel output check compares the drives of two paths",
          "[ntfsundel][cli][drive]") {
  const Fs::path source = "disk.img";
  const Fs::path output = "out";
  const auto ids = [](std::optional<std::string> source_id,
                      std::optional<std::string> output_id) {
    return [=](const Fs::path& path) {
      return path == "disk.img" ? source_id : output_id;
    };
  };

  SECTION("another drive") {
    CHECK(ExampleNtfsUndel::CheckOutputVolume(source, output, ids("a", "b"))
              .empty());
  }

  SECTION("the same drive") {
    const std::wstring message =
        ExampleNtfsUndel::CheckOutputVolume(source, output, ids("a", "a"));
    CHECK(message.find(L"overwrite deleted data") != std::wstring::npos);
    CHECK(message.find(L"out") != std::wstring::npos);
    CHECK(message.find(L"disk.img") != std::wstring::npos);
  }

  SECTION("an output whose drive is not known") {
    CHECK_FALSE(ExampleNtfsUndel::CheckOutputVolume(source, output,
                                                    ids("a", std::nullopt))
                    .empty());
  }

  SECTION("a source whose drive is not known") {
    CHECK_FALSE(ExampleNtfsUndel::CheckOutputVolume(source, output,
                                                    ids(std::nullopt, "b"))
                    .empty());
  }
}

TEST_CASE("NtfsUndel formats one line per file", "[ntfsundel][cli][list]") {
  DeletedFile file = Freed(19, L"old.tmp", L"\\Docs\\old.tmp");
  ExampleNtfsUndel::FileState state;
  state.readable = true;
  state.has_data = true;
  state.size = 16;

  SECTION("intact") {
    CHECK(ExampleNtfsUndel::StateName(state) == L"intact");
    const std::wstring line = ExampleNtfsUndel::FormatListLine(3, file, state);
    CHECK(line.starts_with(L"   3        19            16  intact"));
    CHECK(line.ends_with(L"  \\Docs\\old.tmp"));
    CHECK(line.find(ExampleNtfsUndel::FormatLocalTime(0)) !=
          std::wstring::npos);
  }

  SECTION("each cause has a word") {
    state.check = {.advice = ExampleNtfsUndel::RecoveryAdvice::Confirm,
                   .cause = ExampleNtfsUndel::RecoveryCause::Reallocated};
    CHECK(ExampleNtfsUndel::StateName(state) == L"reallocated");
    CHECK(state.IsDoubtful());
    state.check.cause = ExampleNtfsUndel::RecoveryCause::BadClusters;
    CHECK(ExampleNtfsUndel::StateName(state) == L"bad");
    state.check = {.advice = ExampleNtfsUndel::RecoveryAdvice::Warn,
                   .cause = ExampleNtfsUndel::RecoveryCause::Unchecked};
    CHECK(ExampleNtfsUndel::StateName(state) == L"unchecked");
    CHECK_FALSE(state.IsDoubtful());
  }

  SECTION("a record with no data, and one that does not open") {
    state.has_data = false;
    CHECK(ExampleNtfsUndel::StateName(state) == L"no-data");
    state.readable = false;
    CHECK(ExampleNtfsUndel::StateName(state) == L"unreadable");
  }

  SECTION("a huge file is doubtful") {
    state.size = ExampleNtfsUndel::kConfirmFileSize + 1;
    CHECK(state.IsDoubtful());
  }
}

TEST_CASE("NtfsUndel lists the deleted files of an image without --output",
          "[ntfsundel][cli][list]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const Run run = RunUndel(image.path, {}, Identity(image.path));

  CHECK(run.exit_code == 0);
  const std::string out = run.out;
  CHECK(std::ranges::count(out, '\n') == 4);
  CHECK(out.starts_with("   1        19            16  intact"));
  CHECK(out.find("\\Docs\\old.tmp\n") != std::string::npos);
  CHECK(out.find("\\Docs\\OldDir\n") != std::string::npos);
  CHECK(out.find("  no-data      ") != std::string::npos);
  CHECK(out.find("\\<lost>\\stale.txt\n") != std::string::npos);
  CHECK(run.err.find("4 deleted files") != std::string::npos);
}

TEST_CASE("NtfsUndel restricts the list to a folder inside the volume",
          "[ntfsundel][cli][list]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const std::wstring target = image.path.wstring() + L"/Docs";
  TestIo::Argv argv({target});
  std::wstring error;
  const std::optional<Arguments> args =
      ExampleNtfsUndel::ParseArguments(argv.Count(), argv.Data(), error);
  REQUIRE(args.has_value());
  std::istringstream in;

  const Run run = TestIo::Capture([&](std::FILE* out, std::FILE* err) {
    return ExampleNtfsUndel::UndelTarget(*args,
                                         {.out = out,
                                          .err = err,
                                          .in = &in,
                                          .interactive = false,
                                          .volume_id = Identity(image.path)});
  });

  CHECK(run.exit_code == 0);
  CHECK(std::ranges::count(run.out, '\n') == 3);
  CHECK(run.out.find("old.tmp") != std::string::npos);
  CHECK(run.out.find("OldDir") != std::string::npos);
  CHECK(run.out.find("stale.txt") == std::string::npos);
}

TEST_CASE("NtfsUndel reports a target that is not a volume",
          "[ntfsundel][cli][list]") {
  const TempDir directory;
  const Fs::path file =
      directory.Write("not-ntfs.img", std::string(65536, 'x'));

  const Run run = RunUndel(file, {}, Identity(file));

  CHECK(run.exit_code == ExampleNtfsUndel::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err.find("Cannot read an NTFS volume") != std::string::npos);
}

TEST_CASE("NtfsUndel recovers the files that --record picks",
          "[ntfsundel][cli][recover]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;
  const Fs::path output = directory.Path() / "recovered";

  const Run run =
      RunUndel(image.path, {L"--output", output.wstring(), L"--record", L"19"},
               Identity(image.path));

  CHECK(run.exit_code == 0);
  CHECK(run.err.empty());
  CHECK(Content(output / "old.tmp") ==
        std::string(TestFakeImage::freed_file_content));
  CHECK(run.out.find("Recovered record 19 to ") != std::string::npos);
  CHECK(run.out.find("1 recovered, 0 failed\n") != std::string::npos);
  CHECK_FALSE(Fs::exists(output / "stale.txt"));
}

TEST_CASE("NtfsUndel recovers by name and keeps both files of one name",
          "[ntfsundel][cli][recover]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;
  directory.Write("old.tmp", "mine");

  const Run run =
      RunUndel(image.path,
               {L"--output", directory.Path().wstring(), L"--name", L"OLD.*"},
               Identity(image.path));

  CHECK(run.exit_code == 0);
  CHECK(Content(directory.Path() / "old.tmp") == "mine");
  CHECK(Content(directory.Path() / "old (2).tmp") ==
        std::string(TestFakeImage::freed_file_content));
  CHECK(run.out.find("(a file of that name was already there)") !=
        std::string::npos);
}

TEST_CASE("NtfsUndel --name finds a non-ASCII name by its other case",
          "[ntfsundel][cli][recover]") {
  struct Names {
    std::wstring stored;
    std::wstring pattern;
  };

  const std::vector<Names> cases{{L"été.txt", L"ÉTÉ.*"},
                                 {L"ÉTÉ.txt", L"été.TXT"},
                                 {L"αβ.txt", L"ΑΒ.txt"},
                                 {L"Дом.txt", L"д?м.*"}};
  for (const Names& names : cases) {
    INFO(ToolDeviceAccess::WideToUtf8(names.stored));
    const TempImage image(
        TestFakeImage::BuildFakeNtfsImageWithFreedFileName(names.stored));
    const TempDir directory;

    const Run run = RunUndel(
        image.path,
        {L"--output", directory.Path().wstring(), L"--name", names.pattern},
        Identity(image.path));

    CHECK(run.exit_code == 0);
    CHECK(Content(directory.Path() / Fs::path(names.stored)) ==
          std::string(TestFakeImage::freed_file_content));
  }
}

TEST_CASE("NtfsUndel --name does not match another letter",
          "[ntfsundel][cli][recover]") {
  const TempImage image(
      TestFakeImage::BuildFakeNtfsImageWithFreedFileName(L"été.txt"));
  const TempDir directory;

  const Run run =
      RunUndel(image.path,
               {L"--output", directory.Path().wstring(), L"--name", L"ete.txt"},
               Identity(image.path));

  CHECK(run.exit_code != 0);
  CHECK(run.err.find("No listed file matches") != std::string::npos);
}

TEST_CASE("NtfsUndel --all skips a record with no data",
          "[ntfsundel][cli][recover]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;

  const Run run =
      RunUndel(image.path, {L"-o", directory.Path().wstring(), L"--all"},
               Identity(image.path));

  CHECK(run.err.find("Skipped record 20") != std::string::npos);
  CHECK(Content(directory.Path() / "old.tmp").has_value());
  CHECK_FALSE(Fs::exists(directory.Path() / "OldDir"));
}

TEST_CASE("NtfsUndel refuses an output on the drive that is read",
          "[ntfsundel][cli][drive]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;
  const Fs::path output = directory.Path() / "recovered";

  const Run run =
      RunUndel(image.path, {L"--output", output.wstring(), L"--all"},
               Identity(image.path, true));

  CHECK(run.exit_code == ExampleNtfsUndel::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err.find("overwrite deleted data") != std::string::npos);
  CHECK_FALSE(Fs::exists(output));
}

TEST_CASE("NtfsUndel refuses an output whose drive is not known",
          "[ntfsundel][cli][drive]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;
  const Fs::path output = directory.Path() / "recovered";

  const Run run =
      RunUndel(image.path, {L"--output", output.wstring(), L"--all"},
               [](const Fs::path&) { return std::optional<std::string>(); });

  CHECK(run.exit_code == ExampleNtfsUndel::failure_exit_code);
  CHECK(run.err.find("Cannot tell which drive") != std::string::npos);
  CHECK_FALSE(Fs::exists(output));
}

TEST_CASE("NtfsUndel fails on a record that the list does not hold",
          "[ntfsundel][cli][recover]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;
  const Fs::path output = directory.Path() / "recovered";

  const Run run = RunUndel(
      image.path,
      {L"--output", output.wstring(), L"--record", L"19", L"--record", L"3"},
      Identity(image.path));

  CHECK(run.exit_code == ExampleNtfsUndel::failure_exit_code);
  CHECK(run.err.find("Record 3 is not in the list") != std::string::npos);
  CHECK_FALSE(Fs::exists(output));
}

TEST_CASE("NtfsUndel fails when a name matches no listed file",
          "[ntfsundel][cli][recover]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;

  const Run run = RunUndel(
      image.path,
      {L"--output", directory.Path().wstring(), L"--name", L"*.nothing"},
      Identity(image.path));

  CHECK(run.exit_code == ExampleNtfsUndel::failure_exit_code);
  CHECK(run.err.find("No listed file matches") != std::string::npos);
}

TEST_CASE("NtfsUndel needs a selection when nobody can answer",
          "[ntfsundel][cli][recover]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;

  const Run run =
      RunUndel(image.path, {L"--output", directory.Path().wstring()},
               Identity(image.path), "1\n", false);

  CHECK(run.exit_code == ExampleNtfsUndel::failure_exit_code);
  CHECK(run.err.find("Nothing is selected") != std::string::npos);
  CHECK_FALSE(Content(directory.Path() / "old.tmp").has_value());
}

TEST_CASE("NtfsUndel asks for numbers on a terminal",
          "[ntfsundel][cli][interactive]") {
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;

  SECTION("a valid list") {
    const Run run =
        RunUndel(image.path, {L"--output", directory.Path().wstring()},
                 Identity(image.path), "1\n", true);

    CHECK(run.exit_code == 0);
    CHECK(run.out.find("Files to recover") != std::string::npos);
    CHECK(Content(directory.Path() / "old.tmp") ==
          std::string(TestFakeImage::freed_file_content));
  }

  SECTION("a bad list asks again") {
    const Run run =
        RunUndel(image.path, {L"--output", directory.Path().wstring()},
                 Identity(image.path), "9\nsoon\n1\n", true);

    CHECK(run.exit_code == 0);
    CHECK(run.err.find("Not a valid list") != std::string::npos);
    CHECK(Content(directory.Path() / "old.tmp").has_value());
  }

  SECTION("an empty line quits") {
    const Run run =
        RunUndel(image.path, {L"--output", directory.Path().wstring()},
                 Identity(image.path), "\n", true);

    CHECK(run.exit_code == 0);
    CHECK_FALSE(Content(directory.Path() / "old.tmp").has_value());
  }

  SECTION("the end of the input quits") {
    const Run run =
        RunUndel(image.path, {L"--output", directory.Path().wstring()},
                 Identity(image.path), "", true);

    CHECK(run.exit_code == 0);
    CHECK_FALSE(Content(directory.Path() / "old.tmp").has_value());
  }

  SECTION("a selection on the command line is not asked about") {
    const Run run =
        RunUndel(image.path,
                 {L"--output", directory.Path().wstring(), L"--record", L"19"},
                 Identity(image.path), "", true);

    CHECK(run.exit_code == 0);
    CHECK(run.out.find("Files to recover") == std::string::npos);
    CHECK(Content(directory.Path() / "old.tmp").has_value());
  }

  SECTION("without --output the list is all there is") {
    const Run run = RunUndel(image.path, {}, Identity(image.path), "1\n", true);

    CHECK(run.exit_code == 0);
    CHECK(run.out.find("Files to recover") == std::string::npos);
  }
}

TEST_CASE("NtfsUndel leaves a doubtful file out without --include-doubtful",
          "[ntfsundel][cli][doubtful]") {
  const TempImage image(ReallocatedImage());
  const TempDir directory;

  SECTION("the list names the state") {
    const Run run = RunUndel(image.path, {}, Identity(image.path));

    CHECK(run.out.find("reallocated") != std::string::npos);
  }

  SECTION("left out, with the exit code 1") {
    const Run run =
        RunUndel(image.path,
                 {L"--output", directory.Path().wstring(), L"--record", L"22"},
                 Identity(image.path));

    CHECK(run.exit_code == ExampleNtfsUndel::failure_exit_code);
    CHECK(run.err.find("--include-doubtful") != std::string::npos);
    CHECK(run.err.find("another file") != std::string::npos);
    CHECK_FALSE(Content(directory.Path() / "stale.txt").has_value());
  }

  SECTION("recovered with it, and the warning stays") {
    const Run run = RunUndel(image.path,
                             {L"--output", directory.Path().wstring(),
                              L"--record", L"22", L"--include-doubtful"},
                             Identity(image.path));

    CHECK(run.exit_code == 0);
    CHECK(run.err.find("Warning, record 22") != std::string::npos);
    const std::optional<std::string> bytes =
        Content(directory.Path() / "stale.txt");
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() == 2 * TestFakeImage::fake_cluster_size);
    CHECK(static_cast<unsigned char>((*bytes)[5]) == 5);
  }
}

TEST_CASE("NtfsUndel executable prints usage for --help",
          "[ntfsundel][executable]") {
  const Fs::path exe = NTFS_UNDEL_EXE;
  REQUIRE(Fs::exists(exe));

  const TestSupport::SampleRun run = TestSupport::RunSample(exe, {L"--help"});

  CHECK(run.exit_code == 0);
  CHECK(run.out == ExampleNtfsUndel::UsageText());
  CHECK(run.err.empty());
}

TEST_CASE("NtfsUndel executable prints usage on stderr for a bad command line",
          "[ntfsundel][executable]") {
  const Fs::path exe = NTFS_UNDEL_EXE;
  REQUIRE(Fs::exists(exe));

  const TestSupport::SampleRun run = TestSupport::RunSample(exe, {});

  CHECK(run.exit_code == ExampleNtfsUndel::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err == "A target is needed\n" + ExampleNtfsUndel::UsageText());
}

TEST_CASE("NtfsUndel executable lists the deleted files of an image",
          "[ntfsundel][executable]") {
  const Fs::path exe = NTFS_UNDEL_EXE;
  REQUIRE(Fs::exists(exe));
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());

  const TestSupport::SampleRun run =
      TestSupport::RunSample(exe, {image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out.find("\\Docs\\old.tmp\n") != std::string::npos);
}

TEST_CASE("NtfsUndel executable refuses an output next to the image",
          "[ntfsundel][executable]") {
  const Fs::path exe = NTFS_UNDEL_EXE;
  REQUIRE(Fs::exists(exe));
  const TempImage image(TestFakeImage::BuildFakeNtfsImageWithFreedFileTree());
  const TempDir directory;
  const Fs::path output = directory.Path() / "recovered";

  const TestSupport::SampleRun run = TestSupport::RunSample(
      exe, {image.path.wstring(), L"--output", output.wstring(), L"--all"});

  CHECK(run.exit_code == ExampleNtfsUndel::failure_exit_code);
  CHECK(run.err.find("overwrite deleted data") != std::string::npos);
  CHECK_FALSE(Fs::exists(output));
}
