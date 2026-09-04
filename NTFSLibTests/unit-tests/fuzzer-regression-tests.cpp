// Replays every saved AFL testcase in NTFSLibTests/fuzz/data through the
// actual NtfsFuzzerAfl binary, one file at a time, and checks the process
// exits cleanly. afl-main.cpp always returns 0 unless a real crash (access
// violation, stack overflow, CRT abort, ...) takes the process down, so a
// nonzero/abnormal exit code here means one of these previously-fixed inputs
// has regressed.
//
// NTFSLibTests builds NtfsBrowser with NTFS_BROWSER_ENABLE_TRACE (see
// NTFSLibTests/CMakeLists.txt), so NTFS_TRACE* actually prints to stdout.
// Testcases with a known entry in kExpectedErrorMessages are additionally
// checked for the specific parse-error message the fix for that testcase is
// supposed to produce -- not just "didn't crash", but "failed for the right
// reason". Testcases without an entry only get the exit-code check, because
// they don't reach a distinct NTFS_TRACE'd error (eg. attr_type_slot_aliasing,
// invalid_header_common).

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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

// Keyed by testcase file name (NTFSLibTests/fuzz/data/<key>). Value is every
// substring expected somewhere in the process' stdout, coming from the
// NTFS_TRACE*() call(s) (direct literal, or an e.what() relayed by
// FileRecord::ParseAttrs()) that the corresponding fix added -- a single
// testcase can exercise more than one of a fix's bounds checks in one run.
const std::unordered_map<std::string, std::vector<std::string>>
    kExpectedErrorMessages{
        {"0724c913e1b2f0607bb5cd3ebfacb596db4458e9",
         {"DataRun decode error: run exceeds attribute bounds"}},
        {"65b60629c20b4730c35650dd68b6f87fe57c07a3",
         {"Index Entry Filename name exceeds entry bounds",
          "Index Root: entry_offset exceeds attribute bounds",
          "Index Root: index entry exceeds attribute bounds",
          "Index Block: entry_offset exceeds block bounds"}},
        {"8fc085f7649f977b0ab5f67b5b9da055eebc56dd",
         {"Standard Information attribute smaller than expected."}},
        {"9d6b29a12783a8d0595bf861671e5401493570b5",
         {"Volume Information attribute smaller than expected."}},
        {"f2a2482f50a933eeea4d1a506651884827c0952d",
         {"Index Root attribute smaller than expected."}},
        // FileRecord::ParseAttrs() now rejects an attribute whose
        // total_size is too small for its own header
        // (Attr::HeaderResident/HeaderNonResident) before ever calling
        // ParseAttr() - this testcase's attribute is undersized in exactly
        // that way, so it's now caught earlier than
        // ValidateResidentBounds() (see bug F1 in
        // docs/bug-reports/2026-09-03-full-repo.md).
        {"resident_attr_body_out_of_bounds",
         {"Attribute total_size too small for its header."}},
        // A resident $INDEX_ROOT whose total_size (32) is large enough to
        // pass the F1 check above but whose attr_offset/attr_size (24 + 100)
        // overrun that total_size - exercises ValidateResidentBounds() in
        // AttrResidentNoCache's ctor (src/attr-resident.cpp), the bounds
        // check F1's fix left in place for attributes that declare a
        // consistent total_size but a body extending past it.
        {"resident_attr_body_exceeds_bounds",
         {"Resident attribute body exceeds attribute bounds."}},
        {"cluster_size_null", {"Cluster Size can't be null"}},
        {"invalid_offset_of_us", {"Offset must be lower than 1024."}},
        // Root record (#5) whose offset_of_us (1022) clears the check above
        // but, with sector_size = 1024 (1 sector per 1024-byte record),
        // leaves no room for the 1-word USN fixup array that follows it -
        // exercises the bound FileRecordHeader's ctor now enforces on
        // offset_of_us + 2 * (1 + sectors) (bug F3, see
        // file-record-header-fixup-tests.cpp for the matching unit test).
        {"usn_array_exceeds_record_buffer",
         {"Update Sequence Array does not fit within the file record "
          "buffer."}},
        {"sector_size_too_small", {"Sector Size must be at least 2 bytes"}},
        {"attribute_list_extension_record_cycle",
         {"already resolved in this chain, skipping"}},
    };

struct RunResult
{
  DWORD exit_code = 0;
  std::string output;
};

// Reads a child process' combined stdout/stderr through a pipe while
// waiting for it to exit. The write end must be closed in this process
// after CreateProcess() -- otherwise ReadFile() blocks forever waiting for
// an EOF that can only come once every write handle (including this
// process' own copy) is gone.
std::string ReadAllAndClose(HANDLE readPipe)
{
  std::string output;
  std::array<char, 4096> chunk{};
  DWORD bytesRead = 0;

  while (ReadFile(readPipe, chunk.data(), static_cast<DWORD>(chunk.size()),
                  &bytesRead, nullptr) &&
         bytesRead > 0)
  {
    output.append(chunk.data(), bytesRead);
  }

  CloseHandle(readPipe);
  return output;
}

RunResult RunFuzzerOnFile(const fs::path& exe, const fs::path& testcase)
{
  SECURITY_ATTRIBUTES pipeAttr{};
  pipeAttr.nLength = sizeof(pipeAttr);
  pipeAttr.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  REQUIRE(CreatePipe(&readPipe, &writePipe, &pipeAttr, 0));
  // The read end is only ever used by this process; without this, the
  // child would inherit it too and its own copy of the write end staying
  // open (via the inherited read handle's sibling) could deadlock the
  // ReadAllAndClose() loop above.
  REQUIRE(SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0));

  std::wstring cmdLine =
      L"\"" + exe.wstring() + L"\" \"" + testcase.wstring() + L"\"";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};

  const BOOL created = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr,
                                      TRUE, 0, nullptr, nullptr, &si, &pi);
  // This process' copy of the write end must close regardless of whether
  // CreateProcess succeeded, or ReadAllAndClose() below hangs.
  CloseHandle(writePipe);
  REQUIRE(created);

  const std::string output = ReadAllAndClose(readPipe);

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return {exitCode, output};
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
      const RunResult result = RunFuzzerOnFile(exe, file);
      CHECK(result.exit_code == 0);

      const auto it = kExpectedErrorMessages.find(file.filename().string());
      if (it != kExpectedErrorMessages.end())
      {
        INFO("captured stdout:\n" << result.output);
        for (const std::string& message : it->second)
        {
          CHECK_THAT(result.output,
                     Catch::Matchers::ContainsSubstring(message));
        }
      }
    }
  }
}
