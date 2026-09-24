#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <frozen/bits/elsa_std.h>
#include <frozen/unordered_map.h>
#include <windows.h>

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


#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
inline constexpr bool kDecompressionEnabled = true;
#else
inline constexpr bool kDecompressionEnabled = false;
#endif

inline constexpr std::size_t kMaxExpectedMessages = 7;
using MessageList = std::array<std::string_view, kMaxExpectedMessages>;

struct ExpectedMessages
{
  bool check_expected_messages;
  MessageList messages;
};

constexpr frozen::unordered_map<std::string_view, ExpectedMessages, 74>
    kExpectedErrorMessages{
        {"0724c913e1b2f0607bb5cd3ebfacb596db4458e9",
         {true, {"DataRun decode error: run exceeds attribute bounds"}}},
        {"65b60629c20b4730c35650dd68b6f87fe57c07a3",
         {true,
          {"Index Entry Filename name exceeds entry bounds",
           "Index Root: entry_offset exceeds attribute bounds",
           "Index Root: index entry exceeds attribute bounds",
           "Index Block: entry_offset exceeds block bounds"}}},
        {"8fc085f7649f977b0ab5f67b5b9da055eebc56dd",
         {true, {"Standard Information attribute smaller than expected."}}},
        {"9d6b29a12783a8d0595bf861671e5401493570b5",
         {true, {"Volume Information attribute smaller than expected."}}},
        {"f2a2482f50a933eeea4d1a506651884827c0952d",
         {true, {"Index Root attribute smaller than expected."}}},
        {"resident_attr_body_out_of_bounds",
         {true, {"Attribute total_size too small for its header."}}},
        {"resident_attr_body_exceeds_bounds",
         {true, {"Resident attribute body exceeds attribute bounds."}}},
        {"cluster_size_null", {true, {"Cluster Size can't be null"}}},
        {"invalid_offset_of_us", {true, {"Offset must be lower than 1024."}}},
        {"usn_array_exceeds_record_buffer",
         {true,
          {"Update Sequence Array does not fit within the file record "
           "buffer."}}},
        {"index_block_offset_of_us_out_of_bounds",
         {true, {"Index Block parse error: offset_of_us out of bounds"}}},
        {"file_record_size_invalid", {true, {"FileRecord Size is invalid"}}},
        {"index_block_size_invalid", {true, {"IndexBlock Size is invalid"}}},
        {"file_record_size_shift_overflow",
         {true, {"clusters_per_file_record magnitude out of range"}}},
        {"index_block_size_shift_overflow",
         {true, {"clusters_per_index_block magnitude out of range"}}},
        {"file_record_size_too_big",
         {true,
          {"FileRecord Size exceeds the maximum supported file record "
           "size"}}},
        {"sector_size_too_small",
         {true, {"Sector Size must be at least 2 bytes"}}},
        {"attribute_list_extension_record_cycle",
         {true, {"already resolved in this chain, skipping"}}},
        {"attribute_list_short_read",
         {true,
          {"Attribute List: ReadData returned 10 bytes, expected 26 - "
           "stopping"}}},
        {"attribute_list_multi_type_same_record",
         {true,
          {"Attribute List: record 6, type 0x0090 already resolved in this "
           "chain, skipping"}}},
        {"mft_data_run_cluster_lcn_narrowing_error",
         {true,
          {"Cannot read cluster with LCN", "narrowing_error",
           "Attribute Parse error: 0x0020"}}},
        {"fragmented_record_header_factory_throw",
         {true,
          {"Offset must be lower than 1024.",
           "Attribute Parse error: 0x0020"}}},
        {"mft_addr_narrowing_error", {true, {"MFT address is invalid"}}},
        {"attr_name_exceeds_total_size",
         {true, {"Attribute name exceeds attribute bounds."}}},
        {"attr_offset_exceeds_record_size",
         {true, {"Offset of attr must be within the file record buffer"}}},
        {"volume_information_minimal_size",
         {true, {"NTFS volume version: 3.1"}}},
        {"root_record_parse_failure",
         {true,
          {"IsDeleted() called on a FileRecord with no parsed record",
           "IsDirectory() called on a FileRecord with no parsed record"}}},
        {"find_stream_named_data",
         {true, {"FindStream() found stream named \"ads-name\""}}},
        {"index_root_real_entry",
         {true, {"Index Root: allocated independent copy of resident data"}}},
        {"gap_collation_subnode",
         {true, {"VisitIndexBlock() found entry in sub-node"}}},
        {"standard_information_minimal_size",
         {true, {"Attribute: Standard Information"}}},
        {"index_block_chain_depth_limit",
         {true,
          {"VisitIndexBlock() aborting: recursion depth limit exceeded",
           "TraverseSubNode() aborting: recursion depth limit exceeded"}}},
        {"compressed_index_allocation",
         {kDecompressionEnabled,
          {"Decompressed compression unit 0 into 1024 bytes",
           "per compression unit", "Compressed size = "}}},
        {"surrogate_pair_names",
         {true,
          kDecompressionEnabled
              ? MessageList{
                    "File Name: \xF0\x93\x82\x80",
                    "File Name: \xF0\x9F\x90\x9C",
                    "File Name: "
                    "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                    "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6",
                    "File Name: \xF0\xA0\xAE\xB7",
                    "File Permission: Directory", "File Permission: File",
                    "Decompressed compression unit 0 into 1024 bytes"}
              : MessageList{
                    "File Name: \xF0\x93\x82\x80",
                    "File Name: \xF0\x9F\x90\x9C",
                    "File Permission: Directory", "File Permission: File",
                    "Compressed attribute rejected: decompression is not "
                    "compiled in."}}},
        {"corrupt_compressed_index_allocation",
         {kDecompressionEnabled,
          {"Cannot decompress compression unit 0",
           "LZNT1: back-reference before start of chunk.",
           "per compression unit", "Compressed size = "}}},
        {"compressed_index_allocation_comp_unit_size_out_of_range",
         {true,
          kDecompressionEnabled
              ? MessageList{"Compression unit size is out of range.",
                            "Attribute Parse error: 0x00A0"}
              : MessageList{
                    "Attribute Parse error: 0x00A0",
                    "Compressed attribute rejected: decompression is not "
                    "compiled in."}}},
        {"compressed_index_allocation_oversized_compression_unit",
         {true,
          kDecompressionEnabled
              ? MessageList{"Compression unit size is implausibly large.",
                            "Attribute Parse error: 0x00A0"}
              : MessageList{
                    "Attribute Parse error: 0x00A0",
                    "Compressed attribute rejected: decompression is not "
                    "compiled in."}}},
        {"compressed_index_allocation_misaligned_start_vcn",
         {true,
          kDecompressionEnabled
              ? MessageList{
                    "Compressed attribute start VCN is not compression "
                    "unit aligned.",
                    "Attribute Parse error: 0x00A0"}
              : MessageList{
                    "Attribute Parse error: 0x00A0",
                    "Compressed attribute rejected: decompression is not "
                    "compiled in."}}},
        {"compressed_index_allocation_missing_compressed_size",
         {true,
          {"Compressed attribute total_size too small for its compressed "
           "size field."}}},
        {"compressed_index_allocation_unmapped_unit",
         {kDecompressionEnabled,
          {"Compression unit at VCN 0 is not fully mapped"}}},
        {"compressed_index_allocation_real_after_hole",
         {kDecompressionEnabled,
          {"Compression unit at VCN 0 has real clusters after a hole"}}},
        {"compressed_index_allocation_sparse_unit",
         {kDecompressionEnabled, {"Compression unit 0 is sparse"}}},
        {"compressed_index_allocation_short_decompressed_unit",
         {kDecompressionEnabled,
          {"Decompressed compression unit 0 into 100 bytes",
           "Compression unit 0 decompressed to 100 bytes, expected at "
           "least 4096"}}},
        {"compressed_index_allocation_stored_unit_bad_lcn",
         {kDecompressionEnabled, {"Cannot read stored compression unit 0"}}},
        {"compressed_index_allocation_compressed_unit_bad_lcn",
         {kDecompressionEnabled,
          {"Cannot read compressed compression unit 0"}}},
        {"compressed_index_allocation_lznt1_invalid_signature",
         {kDecompressionEnabled,
          {"LZNT1: invalid chunk header signature.",
           "Cannot decompress compression unit 0"}}},
        {"compressed_index_allocation_lznt1_chunk_exceeds_src_bounds",
         {kDecompressionEnabled,
          {"LZNT1: chunk exceeds compressed data bounds.",
           "Cannot decompress compression unit 0"}}},
        {"compressed_index_allocation_lznt1_uncompressed_chunk_exceeds_dest",
         {kDecompressionEnabled,
          {"LZNT1: uncompressed chunk exceeds decompressed bounds.",
           "Cannot decompress compression unit 0"}}},
        {"compressed_index_allocation_lznt1_chunk_over_4096",
         {kDecompressionEnabled,
          {"LZNT1: chunk decompresses to more than 4096 bytes.",
           "Cannot decompress compression unit 0"}}},
        {"compressed_index_allocation_lznt1_literal_exceeds_dest",
         {kDecompressionEnabled,
          {"LZNT1: literal exceeds decompressed bounds.",
           "Cannot decompress compression unit 0"}}},
        {"compressed_index_allocation_lznt1_truncated_word",
         {kDecompressionEnabled,
          {"LZNT1: truncated compressed word.",
           "Cannot decompress compression unit 0"}}},
        {"compressed_index_allocation_lznt1_backreference_exceeds_dest",
         {kDecompressionEnabled,
          {"LZNT1: back-reference exceeds decompressed bounds.",
           "Cannot decompress compression unit 0"}}},
        {"index_block_magic_mismatch",
         {true, {"Index Block parse error: Magic mismatch"}}},
        {"index_alloc_block_count_incalculable",
         {true, {"Cannot calulate number of IndexBlocks"}}},
        {"index_root_view_not_supported",
         {true, {"Index View not supported"}}},
        {"index_block_usn_mismatch",
         {true, {"Index Block parse error: Update Sequence Number"}}},
        {"index_block_entry_header_exceeds_bounds",
         {true, {"Index Block: index entry header exceeds block bounds"}}},
        {"index_root_entry_header_exceeds_bounds",
         {true,
          {"Index Root: index entry header exceeds attribute bounds"}}},
        {"data_run_decode_error_size_byte",
         {true, {"DataRun decode error 1: 0x"}}},
        {"index_block_entry_exceeds_bounds",
         {true, {"Index Block: index entry exceeds block bounds"}}},
        {"data_run_decode_error_second",
         {true, {"DataRun decode error 2"}}},
        {"data_run_vcn_exceeds_bound",
         {true, {"DataRun decode error: VCN exceeds bound"}}},
        {"data_run_cluster_exceeds_bounds",
         {true, {"Cluster exceeds DataRun bounds"}}},
        {"index_entry_no_filename_stream",
         {true, {"No Filename stream found"}}},
        {"index_entry_stream_smaller_than_expected",
         {true, {"Index Entry stream smaller than expected"}}},
        {"index_entry_stream_exceeds_bounds",
         {true, {"Index Entry stream exceeds entry bounds"}}},
        {"file_record_read_failure",
         {true, {"Cannot read file record 5"}}},
        {"file_record_invalid_magic", {true, {"Invalid file record"}}},
        {"file_record_usn_mismatch",
         {true, {"Update Sequence Number error"}}},
        {"boot_sector_read_failure", {true, {"Read boot sector error"}}},
        {"file_reader_read_failure",
         {true, {"Cannot read file at adress"}}},
        {"traverse_attrs_empty_callback",
         {true, {"TraverseAttrs() called with an empty callback"}}},
        {"file_record_unhandled_attribute",
         {true, {"Unhandled attribute: 0x0040"}}},
        {"bitmap_resident_data_read",
         {true, {"8 bytes of resident Bitmap data read"}}},
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

  std::wstring cmdLine = L"\"" + exe.wstring() +
                         L"\" --inject-read-failures \"" + testcase.wstring() +
                         L"\"";

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
      if (it != kExpectedErrorMessages.end() && it->second.check_expected_messages)
      {
        INFO("captured output:\n" << result.output);
        for (const std::string_view message : it->second.messages)
        {
          // Trailing array slots past this testcase's own messages are
          // empty padding; stop there instead of matching real content.
          if (message.empty())
          {
            break;
          }
          CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring(
                                         std::string(message)));
        }
      }
    }
  }
}
