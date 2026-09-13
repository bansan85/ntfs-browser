#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace
{

// Lists the saved AFL testcases under NTFS_FUZZ_DATA_DIR, sorted.
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

// Runs exe on testcase and returns its exit code; nonzero means it crashed.
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

}

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
