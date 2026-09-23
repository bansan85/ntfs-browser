// Tests for the runtime logging configuration (include/ntfs-browser/log.h):
// the --log option parser, per-target levels, and the console target's
// split between stdout and stderr.
//
// The split cannot be observed in process: on Windows spdlog's console
// sinks cache the HANDLE that GetStdHandle() returned when they were
// built, so redirecting a CRT file descriptor changes nothing. The split
// is therefore checked by running NtfsFuzzerAfl - which drives the library
// over a saved corpus file and takes --log itself - with its two standard
// streams sent to two separate files.

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <windows.h>

#include <ntfs-browser/log.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfs-common.h"
#include "test-log-sink.h"

namespace fs = std::filesystem;

using Catch::Matchers::ContainsSubstring;
using NtfsBrowser::Log::Config;
using NtfsBrowser::Log::Level;

namespace
{

// Corpus testcase driven by the child-process cases below. Its run logs an
// info line (the sector size) and an error line (the null cluster size),
// so one run exercises both halves of the console split.
constexpr std::string_view kSplitTestcase = "cluster_size_null";
// Substrings those two lines are recognised by.
constexpr std::string_view kInfoLine = "Sector Size = ";
constexpr std::string_view kErrorLine = "Cluster Size can't be null";

// Puts the trace-level capturing sink back once a test has replaced the
// library logger's sinks with a configuration of its own.
class RestoreCaptureSink
{
 public:
  RestoreCaptureSink() = default;
  RestoreCaptureSink(RestoreCaptureSink&&) = delete;
  RestoreCaptureSink(const RestoreCaptureSink&) = delete;
  RestoreCaptureSink& operator=(RestoreCaptureSink&&) = delete;
  RestoreCaptureSink& operator=(const RestoreCaptureSink&) = delete;
  ~RestoreCaptureSink() { NtfsBrowserTests::InstallCaptureSink(); }
};

// A path in the temp directory that no other test uses, removed again by
// the destructor.
class TempFile
{
 public:
  explicit TempFile(std::wstring_view tag)
      : path_(fs::temp_directory_path() /
              (L"ntfsbrowser-log-" + std::wstring(tag) + L"-" +
               std::to_wstring(GetCurrentProcessId()) + L".txt"))
  {
    std::error_code ec;
    fs::remove(path_, ec);
  }

  TempFile(TempFile&&) = delete;
  TempFile(const TempFile&) = delete;
  TempFile& operator=(TempFile&&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  ~TempFile()
  {
    std::error_code ec;
    fs::remove(path_, ec);
  }

  [[nodiscard]] const fs::path& Path() const noexcept { return path_; }

  [[nodiscard]] std::string Read() const
  {
    std::ifstream in(path_, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }

 private:
  fs::path path_;
};

// Opens path for the child process to inherit as a standard stream.
HANDLE CreateInheritableOutput(const fs::path& path)
{
  SECURITY_ATTRIBUTES attr{};
  attr.nLength = sizeof(attr);
  attr.bInheritHandle = TRUE;

  return CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attr,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

struct ChildOutput
{
  DWORD exit_code = 0;
  std::string out;
  std::string err;
};

// Runs NtfsFuzzerAfl on the corpus testcase with extraArgs appended, and
// returns its two standard streams separately. Files rather than pipes,
// so neither stream can fill a pipe buffer and deadlock the other.
ChildOutput RunFuzzer(const std::wstring& extraArgs)
{
  const fs::path exe(NTFS_FUZZER_AFL_EXE);
  const fs::path testcase =
      fs::path(NTFS_FUZZ_DATA_DIR) / std::string(kSplitTestcase);
  REQUIRE(fs::exists(exe));
  REQUIRE(fs::exists(testcase));

  const TempFile outFile(L"stdout");
  const TempFile errFile(L"stderr");

  HANDLE outHandle = CreateInheritableOutput(outFile.Path());
  HANDLE errHandle = CreateInheritableOutput(errFile.Path());
  REQUIRE(outHandle != INVALID_HANDLE_VALUE);
  REQUIRE(errHandle != INVALID_HANDLE_VALUE);

  std::wstring cmdLine = L"\"" + exe.wstring() + L"\" \"" + testcase.wstring() +
                         L"\" " + extraArgs;

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = outHandle;
  si.hStdError = errHandle;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};

  const BOOL created = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr,
                                      TRUE, 0, nullptr, nullptr, &si, &pi);
  CloseHandle(outHandle);
  CloseHandle(errHandle);
  REQUIRE(created);

  WaitForSingleObject(pi.hProcess, INFINITE);

  ChildOutput result;
  GetExitCodeProcess(pi.hProcess, &result.exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  result.out = outFile.Read();
  result.err = errFile.Read();
  return result;
}

}  // namespace

TEST_CASE("the --log option parses a target and a level", "[logging]")
{
  Config config;

  SECTION("console level")
  {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=console:debug", config));
    CHECK(config.console_level == Level::kDebug);
    CHECK(config.file_level == Level::kOff);
  }

  SECTION("file level, default path")
  {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=file:debug", config));
    CHECK(config.file_level == Level::kDebug);
    CHECK(config.file_path == NtfsBrowser::Log::kDefaultFilePath);
    CHECK(config.console_level == Level::kWarn);
  }

  SECTION("file level and path, drive letter kept")
  {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=file:trace:C:\\tmp\\ntfs.log",
                                          config));
    CHECK(config.file_level == Level::kTrace);
    CHECK(config.file_path == "C:\\tmp\\ntfs.log");
  }

  SECTION("wide option, path kept as wide characters")
  {
    REQUIRE(NtfsBrowser::Log::ParseOption(
        L"--log=file:trace:C:\\tmp\\\u30ed.log", config));
    CHECK(config.file_level == Level::kTrace);
    CHECK(config.file_path == fs::path(L"C:\\tmp\\\u30ed.log"));
  }

  SECTION("repeated, once per target")
  {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=console:error", config));
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=file:trace", config));
    CHECK(config.console_level == Level::kError);
    CHECK(config.file_level == Level::kTrace);
  }

  SECTION("either target may be off")
  {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=console:off", config));
    CHECK(config.console_level == Level::kOff);
  }
}

TEST_CASE("the --log option rejects a malformed value and changes nothing",
          "[logging]")
{
  const std::array<const char*, 6> rejected{
      "--log=syslog:debug", "--log=console:verbose", "--log=console", "--log=",
      "console:debug",      "--log=file:trace:"};

  for (const char* option : rejected)
  {
    Config config;
    INFO("option: " << option);
    CHECK_FALSE(NtfsBrowser::Log::ParseOption(option, config));
    CHECK(config.console_level == Level::kWarn);
    CHECK(config.file_level == Level::kOff);
    CHECK(config.file_path == NtfsBrowser::Log::kDefaultFilePath);
  }
}

TEST_CASE("the default configuration logs warnings, not info", "[logging]")
{
  const Config config;
  CHECK(config.console_level == Level::kWarn);
  CHECK(config.file_level == Level::kOff);
  CHECK(config.file_path == NtfsBrowser::Log::kDefaultFilePath);
}

TEST_CASE("the volume name is logged without its terminator", "[logging]")
{
  (void)NtfsBrowserTests::TakeCapturedLog();
  const NtfsBrowser::NtfsVolume<NtfsBrowser::Strategy::NO_CACHE> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithVolumeName()));
  const std::string captured = NtfsBrowserTests::TakeCapturedLog();

  CHECK(volume.IsVolumeOK());
  CHECK_THAT(captured, ContainsSubstring("NTFS volume name: TESTVOL"));
  // AttrVolName pads its buffer with a terminator its view still covers.
  // UTF-8 has no terminator, so a NUL byte must not reach the line.
  CHECK(captured.find('\0') == std::string::npos);
}

TEST_CASE("each sink keeps its own level", "[logging]")
{
  const RestoreCaptureSink restore;
  const TempFile logFile(L"levels");

  Config config;
  config.console_level = Level::kOff;
  config.file_level = Level::kWarn;
  config.file_path = logFile.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));

  NtfsBrowser::LogTrace("trace-only-line");
  NtfsBrowser::LogInfo("info-only-line");
  NtfsBrowser::LogWarn("warn-line");
  NtfsBrowser::LogError("error-line");

  // Drops the file sink, which closes the file before it is read back.
  NtfsBrowserTests::InstallCaptureSink();

  const std::string contents = logFile.Read();
  CHECK_THAT(contents, ContainsSubstring("warn-line"));
  CHECK_THAT(contents, ContainsSubstring("error-line"));
  CHECK_THAT(contents, !ContainsSubstring("trace-only-line"));
  CHECK_THAT(contents, !ContainsSubstring("info-only-line"));
}

TEST_CASE("a file sink at trace records every level", "[logging]")
{
  const RestoreCaptureSink restore;
  const TempFile logFile(L"trace");

  Config config;
  config.console_level = Level::kOff;
  config.file_level = Level::kTrace;
  config.file_path = logFile.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));

  NtfsBrowser::LogTrace("recorded-trace");
  NtfsBrowser::LogError("recorded-error");

  NtfsBrowserTests::InstallCaptureSink();

  const std::string contents = logFile.Read();
  CHECK_THAT(contents, ContainsSubstring("recorded-trace"));
  CHECK_THAT(contents, ContainsSubstring("recorded-error"));
}

TEST_CASE("both targets off writes nothing at all", "[logging]")
{
  const RestoreCaptureSink restore;
  const TempFile logFile(L"silent");

  Config config;
  config.console_level = Level::kOff;
  config.file_level = Level::kOff;
  config.file_path = logFile.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));

  NtfsBrowser::LogError("never-written");

  NtfsBrowserTests::InstallCaptureSink();
  CHECK_FALSE(fs::exists(logFile.Path()));
}

TEST_CASE("a message carrying braces is not treated as a format string",
          "[logging]")
{
  const RestoreCaptureSink restore;
  const TempFile logFile(L"braces");

  Config config;
  config.console_level = Level::kOff;
  config.file_level = Level::kTrace;
  config.file_path = logFile.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));

  // What LogException() relays: runtime text, never a format string.
  const std::runtime_error thrown("relayed {0} {bad} text\n");
  NtfsBrowser::LogException(thrown);

  NtfsBrowserTests::InstallCaptureSink();

  // Carriage returns go first: spdlog ends a line with \r\n here, which
  // would hide the thrower's own newline from the check below.
  std::string contents = logFile.Read();
  std::erase(contents, '\r');

  CHECK_THAT(contents, ContainsSubstring("relayed {0} {bad} text\n"));
  // Leaving the thrower's newline in would put a blank line after it.
  CHECK_THAT(contents, !ContainsSubstring("text\n\n"));
}

TEST_CASE("Configure() on an unwritable path fails without throwing",
          "[logging]")
{
  const RestoreCaptureSink restore;

  Config config;
  config.console_level = Level::kWarn;
  config.file_level = Level::kTrace;
  // A directory that cannot exist, so the sink's fopen() must fail.
  config.file_path = "Z:\\ntfs-browser-no-such-directory\\log.txt";

  CHECK_FALSE(NtfsBrowser::Log::Configure(config));

  // The console target survives the file sink's failure.
  NtfsBrowser::LogError("still-logging");
}

TEST_CASE("a log path outside the ANSI code page still opens", "[logging]")
{
  const RestoreCaptureSink restore;
  // Japanese kana, which no Western Windows ANSI code page can express.
  // The file only opens if the path stays wide from --log through to the
  // sink's fopen().
  const TempFile logFile(L"\u30ed\u30b0");

  Config config;
  REQUIRE(NtfsBrowser::Log::ParseOption(
      std::wstring(L"--log=file:trace:") + logFile.Path().wstring(), config));
  CHECK(config.file_path == logFile.Path());

  config.console_level = Level::kOff;
  REQUIRE(NtfsBrowser::Log::Configure(config));

  NtfsBrowser::LogError("wide-path-line");

  NtfsBrowserTests::InstallCaptureSink();

  CHECK(fs::exists(logFile.Path()));
  CHECK_THAT(logFile.Read(), ContainsSubstring("wide-path-line"));
}

TEST_CASE("Configure() replaces the previous sinks wholesale", "[logging]")
{
  const RestoreCaptureSink restore;
  const TempFile first(L"first");
  const TempFile second(L"second");

  Config config;
  config.console_level = Level::kOff;
  config.file_level = Level::kTrace;
  config.file_path = first.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));
  NtfsBrowser::LogError("into-first");

  config.file_path = second.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));
  NtfsBrowser::LogError("into-second");

  NtfsBrowserTests::InstallCaptureSink();

  CHECK_THAT(first.Read(), ContainsSubstring("into-first"));
  CHECK_THAT(first.Read(), !ContainsSubstring("into-second"));
  CHECK_THAT(second.Read(), ContainsSubstring("into-second"));
}

TEST_CASE("the console target splits by level across the two streams",
          "[logging]")
{
  SECTION("console:warn: warnings on stderr, stdout silent")
  {
    const ChildOutput result = RunFuzzer(L"--log=console:warn");
    CHECK(result.exit_code == 0);
    CHECK_THAT(result.err, ContainsSubstring(std::string(kErrorLine)));
    CHECK_THAT(result.out, !ContainsSubstring(std::string(kErrorLine)));
    CHECK_THAT(result.out, !ContainsSubstring(std::string(kInfoLine)));
  }

  SECTION("console:trace: each line on exactly one stream")
  {
    const ChildOutput result = RunFuzzer(L"--log=console:trace");
    CHECK(result.exit_code == 0);
    CHECK_THAT(result.out, ContainsSubstring(std::string(kInfoLine)));
    CHECK_THAT(result.out, !ContainsSubstring(std::string(kErrorLine)));
    CHECK_THAT(result.err, ContainsSubstring(std::string(kErrorLine)));
    CHECK_THAT(result.err, !ContainsSubstring(std::string(kInfoLine)));
  }

  SECTION("console:error: stdout stays silent")
  {
    const ChildOutput result = RunFuzzer(L"--log=console:error");
    CHECK(result.exit_code == 0);
    CHECK(result.out.empty());
    CHECK_THAT(result.err, ContainsSubstring(std::string(kErrorLine)));
  }

  SECTION("console:off: nothing on either stream")
  {
    const ChildOutput result = RunFuzzer(L"--log=console:off");
    CHECK(result.exit_code == 0);
    CHECK(result.out.empty());
    CHECK(result.err.empty());
  }

  SECTION("an unknown target is rejected with a non-zero exit")
  {
    const ChildOutput result = RunFuzzer(L"--log=syslog:debug");
    CHECK(result.exit_code != 0);
  }

  SECTION("an unknown level is rejected with a non-zero exit")
  {
    const ChildOutput result = RunFuzzer(L"--log=console:verbose");
    CHECK(result.exit_code != 0);
  }
}

TEST_CASE("the file target records what the console target is denied",
          "[logging]")
{
  const TempFile logFile(L"child");

  const ChildOutput result =
      RunFuzzer(L"--log=console:off \"--log=file:trace:" +
                logFile.Path().wstring() + L"\"");
  CHECK(result.exit_code == 0);
  CHECK(result.out.empty());
  CHECK(result.err.empty());

  const std::string contents = logFile.Read();
  CHECK_THAT(contents, ContainsSubstring(std::string(kInfoLine)));
  CHECK_THAT(contents, ContainsSubstring(std::string(kErrorLine)));
}
