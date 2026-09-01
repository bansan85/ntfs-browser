// Replays every saved AFL testcase in NTFSLibTests/fuzz/data through the
// actual NtfsFuzzerAfl binary, one file at a time, and checks the process
// exits cleanly. afl-main.cpp always returns 0 unless a real crash (access
// violation, stack overflow, CRT abort, ...) takes the process down, so a
// nonzero/abnormal exit code here means one of these previously-fixed inputs
// has regressed.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace
{

std::vector<fs::path> ListRegressionTestcases()
{
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(fs::path(NTFS_FUZZ_DATA_DIR)))
  {
    if (entry.is_regular_file())
    {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

DWORD RunFuzzerOnFile(const fs::path& exe, const fs::path& testcase)
{
  std::wstring cmdLine =
      L"\"" + exe.wstring() + L"\" \"" + testcase.wstring() + L"\"";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  REQUIRE(CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, 0,
                         nullptr, nullptr, &si, &pi));

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return exitCode;
}

}  // namespace

TEST_CASE("NtfsFuzzerAfl does not crash on saved regression testcases",
          "[fuzz][regression]")
{
  const fs::path exe(NTFS_FUZZER_AFL_EXE);
  REQUIRE(fs::exists(exe));

  const std::vector<fs::path> files = ListRegressionTestcases();
  REQUIRE_FALSE(files.empty());

  for (const fs::path& file : files)
  {
    DYNAMIC_SECTION("testcase: " << file.filename().string())
    {
      CHECK(RunFuzzerOnFile(exe, file) == 0);
    }
  }
}
