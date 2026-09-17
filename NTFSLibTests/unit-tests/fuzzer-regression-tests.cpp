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
        {"resident_attr_body_out_of_bounds",
         {"Attribute total_size too small for its header."}},
        // total_size passes the check above; attr_offset + attr_size overrun it
        {"resident_attr_body_exceeds_bounds",
         {"Resident attribute body exceeds attribute bounds."}},
        {"cluster_size_null", {"Cluster Size can't be null"}},
        {"invalid_offset_of_us", {"Offset must be lower than 1024."}},
        {"usn_array_exceeds_record_buffer",
         {"Update Sequence Array does not fit within the file record "
          "buffer."}},
        {"index_block_offset_of_us_out_of_bounds",
         {"Index Block parse error: offset_of_us out of bounds"}},
        {"file_record_size_invalid", {"FileRecord Size is invalid"}},
        {"index_block_size_invalid", {"IndexBlock Size is invalid"}},
        {"file_record_size_shift_overflow",
         {"clusters_per_file_record magnitude out of range"}},
        {"index_block_size_shift_overflow",
         {"clusters_per_index_block magnitude out of range"}},
        {"file_record_size_too_big",
         {"FileRecord Size exceeds the maximum supported file record size"}},
        {"sector_size_too_small", {"Sector Size must be at least 2 bytes"}},
        {"attribute_list_extension_record_cycle",
         {"already resolved in this chain, skipping"}},
        {"attribute_list_short_read",
         {"Attribute List: ReadData returned 10 bytes, expected 26 - "
          "stopping"}},
        {"attribute_list_multi_type_same_record",
         {"Attribute List: record 6, type 0x0090 already resolved in this "
          "chain, skipping"}},
        {"mft_data_run_cluster_lcn_narrowing_error",
         {"Cannot read cluster with LCN", "narrowing_error",
          "Attribute Parse error: 0x0020"}},
        {"fragmented_record_header_factory_throw",
         {"Offset must be lower than 1024.", "Attribute Parse error: 0x0020"}},
        {"mft_addr_narrowing_error", {"MFT address is invalid"}},
        {"attr_name_exceeds_total_size",
         {"Attribute name exceeds attribute bounds."}},
        {"attr_offset_exceeds_record_size",
         {"Offset of attr must be within the file record buffer"}},
        {"volume_information_minimal_size", {"NTFS volume version: 3.1"}},
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

// Runs exe on testcase; returns its exit code and everything it printed.
RunResult RunFuzzerOnFile(const fs::path& exe, const fs::path& testcase)
{
  SECURITY_ATTRIBUTES pipeAttr{};
  pipeAttr.nLength = sizeof(pipeAttr);
  pipeAttr.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  REQUIRE(CreatePipe(&readPipe, &writePipe, &pipeAttr, 0));
  // Without this, the child would inherit the read end too, and could
  // deadlock ReadAllAndClose() above by keeping the write end open.
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
  // Must close this process' copy now regardless of success, or
  // ReadAllAndClose() below hangs.
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
