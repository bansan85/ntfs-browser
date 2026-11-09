#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/command-line.h>

namespace {

namespace FuzzGen = NTFS_BROWSER_NAMESPACE::Tool::FuzzGen;

// What CommandLine::Parse() returned and wrote to its error stream.
struct ParseResult {
  std::optional<FuzzGen::Options> options;
  std::string err;
};

// Parses the words as the arguments that follow the program name.
ParseResult ParseFuzzGenArgs(const std::vector<std::string>& words) {
  std::vector<std::filesystem::path> args{"NtfsFuzzGen"};
  args.insert(args.end(), words.begin(), words.end());
  std::ostringstream err;
  ParseResult result;
  result.options = FuzzGen::CommandLine::Parse(args, err);
  result.err = err.str();
  return result;
}

}  // namespace

TEST_CASE("FuzzGen command line accepts one mode", "[fuzz][fuzzgen]") {
  SECTION("no argument asks for the usage") {
    const auto result = ParseFuzzGenArgs({});
    REQUIRE(result.options.has_value());
    CHECK(result.options->mode == FuzzGen::Options::Mode::Help);
    CHECK(result.err.empty());
  }

  SECTION("--list takes no folder") {
    const auto result = ParseFuzzGenArgs({"--list"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->mode == FuzzGen::Options::Mode::List);
    CHECK(result.options->folder.empty());
  }

  SECTION("--list does not eat the option that follows") {
    const auto result = ParseFuzzGenArgs({"--list", "--only", "a"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->mode == FuzzGen::Options::Mode::List);
    CHECK(result.options->only == std::vector<std::string>{"a"});
  }

  SECTION("--out, --seeds and --verify take a folder") {
    const auto out = ParseFuzzGenArgs({"--out", "dir-a"});
    REQUIRE(out.options.has_value());
    CHECK(out.options->mode == FuzzGen::Options::Mode::Out);
    CHECK(out.options->folder == "dir-a");

    const auto seeds = ParseFuzzGenArgs({"--seeds", "dir-b"});
    REQUIRE(seeds.options.has_value());
    CHECK(seeds.options->mode == FuzzGen::Options::Mode::Seeds);
    CHECK(seeds.options->folder == "dir-b");

    const auto verify = ParseFuzzGenArgs({"--verify", "dir-c"});
    REQUIRE(verify.options.has_value());
    CHECK(verify.options->mode == FuzzGen::Options::Mode::Verify);
    CHECK(verify.options->folder == "dir-c");
  }

  SECTION("--predict takes three files") {
    const auto result = ParseFuzzGenArgs({"--predict", "l", "i", "o"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->mode == FuzzGen::Options::Mode::Predict);
    CHECK(result.options->layers == "l");
    CHECK(result.options->intervals == "i");
    CHECK(result.options->output == "o");
  }

  SECTION("--predict takes an operand that looks like an option") {
    const auto result = ParseFuzzGenArgs({"--predict", "--l", "--i", "--o"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->layers == "--l");
    CHECK(result.options->output == "--o");
  }
}

TEST_CASE("FuzzGen command line reads the other options", "[fuzz][fuzzgen]") {
  SECTION("--resources sets the folder, even with a leading dash") {
    const auto result = ParseFuzzGenArgs({"--resources", "--odd", "--list"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->resources == "--odd");
    CHECK(result.options->mode == FuzzGen::Options::Mode::List);
  }

  SECTION("--only collects names up to the next option") {
    const auto result =
        ParseFuzzGenArgs({"--only", "a", "b", "--out", "dir", "--only", "c"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->only == std::vector<std::string>{"a", "b", "c"});
    CHECK(result.options->mode == FuzzGen::Options::Mode::Out);
    CHECK(result.options->folder == "dir");
  }
}

TEST_CASE("FuzzGen command line rejects a wrong command", "[fuzz][fuzzgen]") {
  const std::string one_mode =
      "only one of --list, --out, --seeds and --verify is allowed\n";
  const std::string one_mode_predict =
      "only one of --list, --out, --seeds, --verify and --predict is "
      "allowed\n";

  SECTION("a second mode") {
    auto result = ParseFuzzGenArgs({"--list", "--out", "dir"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == one_mode);

    result = ParseFuzzGenArgs({"--out", "dir", "--list"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == one_mode);

    result = ParseFuzzGenArgs({"--list", "--list"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == one_mode);

    result = ParseFuzzGenArgs({"--predict", "l", "i", "o", "--seeds", "d"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == one_mode);
  }

  SECTION("--predict after a mode") {
    auto result =
        ParseFuzzGenArgs({"--verify", "dir", "--predict", "l", "i", "o"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == one_mode_predict);

    result = ParseFuzzGenArgs(
        {"--predict", "l", "i", "o", "--predict", "l", "i", "o"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == one_mode_predict);
  }

  SECTION("a mode that needs a folder") {
    auto result = ParseFuzzGenArgs({"--out"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == "--out needs a folder\n");

    result = ParseFuzzGenArgs({"--seeds", "--list"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == "--seeds needs a folder\n");

    result = ParseFuzzGenArgs({"--verify"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == "--verify needs a folder\n");
  }

  SECTION("--predict with too few operands") {
    const std::string needs =
        "--predict needs a layers file, an intervals file and an output "
        "file\n";
    auto result = ParseFuzzGenArgs({"--predict"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == needs);

    result = ParseFuzzGenArgs({"--predict", "l", "i"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == needs);
  }

  SECTION("--resources without a folder") {
    const auto result = ParseFuzzGenArgs({"--resources"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == "--resources needs a folder\n");
  }

  SECTION("--only without a name") {
    auto result = ParseFuzzGenArgs({"--only"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == "--only needs at least one name\n");

    result = ParseFuzzGenArgs({"--only", "--list"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == "--only needs at least one name\n");
  }

  SECTION("an unknown argument") {
    auto result = ParseFuzzGenArgs({"--bogus"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == "unknown argument --bogus\n");

    result = ParseFuzzGenArgs({"stray"});
    CHECK_FALSE(result.options.has_value());
    CHECK(result.err == "unknown argument stray\n");
  }
}
