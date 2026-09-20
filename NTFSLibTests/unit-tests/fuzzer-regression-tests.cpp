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
        {"root_record_parse_failure",
         {"IsDeleted() called on a FileRecord with no parsed record",
          "IsDirectory() called on a FileRecord with no parsed record"}},
        {"find_stream_named_data",
         {"FindStream() found stream named \"ads-name\""}},
        {"index_root_real_entry",
         {"Index Root: allocated independent copy of resident data"}},
        {"gap_collation_subnode",
         {"VisitIndexBlock() found entry in sub-node"}},
        {"standard_information_minimal_size",
         {"Attribute: Standard Information"}},
        {"index_block_chain_depth_limit",
         {"VisitIndexBlock() aborting: recursion depth limit exceeded",
          "TraverseSubNode() aborting: recursion depth limit exceeded"}},
        {"compressed_index_allocation",
         {"Decompressed compression unit 0 into 1024 bytes",
          "per compression unit", "Compressed size = "}},
        {"corrupt_compressed_index_allocation",
         {"Cannot decompress compression unit 0",
          "LZNT1: back-reference before start of chunk.",
          "per compression unit", "Compressed size = "}},
        {"compressed_index_allocation_comp_unit_size_out_of_range",
         {"Compression unit size is out of range.",
          "Attribute Parse error: 0x00A0"}},
        {"compressed_index_allocation_oversized_compression_unit",
         {"Compression unit size is implausibly large.",
          "Attribute Parse error: 0x00A0"}},
        {"compressed_index_allocation_misaligned_start_vcn",
         {"Compressed attribute start VCN is not compression unit aligned.",
          "Attribute Parse error: 0x00A0"}},
        {"compressed_index_allocation_missing_compressed_size",
         {"Compressed attribute total_size too small for its compressed "
          "size field."}},
        {"compressed_index_allocation_unmapped_unit",
         {"Compression unit at VCN 0 is not fully mapped"}},
        {"compressed_index_allocation_real_after_hole",
         {"Compression unit at VCN 0 has real clusters after a hole"}},
        {"compressed_index_allocation_sparse_unit",
         {"Compression unit 0 is sparse"}},
        {"compressed_index_allocation_short_decompressed_unit",
         {"Decompressed compression unit 0 into 100 bytes",
          "Compression unit 0 decompressed to 100 bytes, expected at least "
          "4096"}},
        {"compressed_index_allocation_stored_unit_bad_lcn",
         {"Cannot read stored compression unit 0"}},
        {"compressed_index_allocation_compressed_unit_bad_lcn",
         {"Cannot read compressed compression unit 0"}},
        {"compressed_index_allocation_encrypted",
         {"Encrypted file not supported yet !"}},
        {"compressed_index_allocation_lznt1_invalid_signature",
         {"LZNT1: invalid chunk header signature.",
          "Cannot decompress compression unit 0"}},
        {"compressed_index_allocation_lznt1_chunk_exceeds_src_bounds",
         {"LZNT1: chunk exceeds compressed data bounds.",
          "Cannot decompress compression unit 0"}},
        {"compressed_index_allocation_lznt1_uncompressed_chunk_exceeds_dest",
         {"LZNT1: uncompressed chunk exceeds decompressed bounds.",
          "Cannot decompress compression unit 0"}},
        {"compressed_index_allocation_lznt1_chunk_over_4096",
         {"LZNT1: chunk decompresses to more than 4096 bytes.",
          "Cannot decompress compression unit 0"}},
        {"compressed_index_allocation_lznt1_literal_exceeds_dest",
         {"LZNT1: literal exceeds decompressed bounds.",
          "Cannot decompress compression unit 0"}},
        {"compressed_index_allocation_lznt1_truncated_word",
         {"LZNT1: truncated compressed word.",
          "Cannot decompress compression unit 0"}},
        {"compressed_index_allocation_lznt1_backreference_exceeds_dest",
         {"LZNT1: back-reference exceeds decompressed bounds.",
          "Cannot decompress compression unit 0"}},
        {"index_block_magic_mismatch",
         {"Index Block parse error: Magic mismatch"}},
        {"index_alloc_block_count_incalculable",
         {"Cannot calulate number of IndexBlocks"}},
        {"index_root_view_not_supported", {"Index View not supported"}},
        {"index_block_usn_mismatch",
         {"Index Block parse error: Update Sequence Number"}},
        {"index_block_entry_header_exceeds_bounds",
         {"Index Block: index entry header exceeds block bounds"}},
        {"index_root_entry_header_exceeds_bounds",
         {"Index Root: index entry header exceeds attribute bounds"}},
        {"data_run_decode_error_size_byte", {"DataRun decode error 1: 0x"}},
        {"index_block_entry_exceeds_bounds",
         {"Index Block: index entry exceeds block bounds"}},
        {"data_run_decode_error_second", {"DataRun decode error 2"}},
        {"data_run_vcn_exceeds_bound",
         {"DataRun decode error: VCN exceeds bound"}},
        {"data_run_cluster_exceeds_bounds", {"Cluster exceeds DataRun bounds"}},
        {"index_entry_no_filename_stream", {"No Filename stream found"}},
        {"index_entry_stream_smaller_than_expected",
         {"Index Entry stream smaller than expected"}},
        {"index_entry_stream_exceeds_bounds",
         {"Index Entry stream exceeds entry bounds"}},
        {"file_record_read_failure", {"Cannot read file record 5"}},
        {"file_record_invalid_magic", {"Invalid file record"}},
        {"file_record_usn_mismatch", {"Update Sequence Number error"}},
        {"boot_sector_read_failure", {"Read boot sector error"}},
        {"file_reader_read_failure", {"Cannot read file at adress"}},
        {"traverse_attrs_empty_callback",
         {"TraverseAttrs() called with an empty callback"}},
        {"file_record_unhandled_attribute", {"Unhandled attribute: 0x0040"}},
        {"bitmap_resident_data_read", {"8 bytes of resident Bitmap data read"}},
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
