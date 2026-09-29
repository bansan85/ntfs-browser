#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <frozen/bits/elsa_std.h>
#include <frozen/unordered_map.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "child-process.h"

using NtfsBrowserTests::ProcessOutput;
using NtfsBrowserTests::RunProcessCapturingOutput;

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

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
inline constexpr bool kEfsEnabled = true;
#else
inline constexpr bool kEfsEnabled = false;
#endif

inline constexpr std::size_t kMaxExpectedMessages = 7;
using MessageList = std::array<std::string_view, kMaxExpectedMessages>;

struct ExpectedMessages
{
  bool check_expected_messages;
  MessageList messages;
};

constexpr frozen::unordered_map<std::string_view, ExpectedMessages, 103>
    kExpectedErrorMessages{
        {"0724c913e1b2f0607bb5cd3ebfacb596db4458e9",
         {true,
          {"DataRun decode error: run exceeds attribute bounds",
           "Data run is malformed."}}},
        {"65b60629c20b4730c35650dd68b6f87fe57c07a3",
         {true,
          {"Index Entry Filename name exceeds entry bounds",
           "Index Root: entry_offset exceeds attribute bounds",
           "Index Root: index entry exceeds attribute bounds",
           "Index Block: entry_offset exceeds block bounds",
           "Index Root attribute has a malformed index entry."}}},
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
           "stopping",
           "Attribute List is truncated."}}},
        {"attribute_list_multi_type_same_record",
         {true,
          {"Attribute List: record 6, type 0x0090 already resolved in this "
           "chain, skipping"}}},
        {"mft_data_run_cluster_lcn_narrowing_error",
         {true,
          {"Cannot read cluster with LCN", "narrowing_error",
           "Attribute Parse error: 0x0020",
           "Attribute List parse error (ParseFileRecord)."}}},
        {"fragmented_record_header_factory_throw",
         {true,
          {"Offset must be lower than 1024.", "Attribute Parse error: 0x0020",
           "Attribute List parse error (ParseFileRecord)."}}},
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
           "TraverseSubNode() aborting: recursion depth limit exceeded",
           "TraverseSubEntries() recovery: reporting orphaned index block"}}},
        {"compressed_index_allocation",
         {kDecompressionEnabled,
          {"Decompressed compression unit 0 into 1024 bytes",
           "per compression unit", "Compressed size = "}}},
        {"surrogate_pair_names",
         {true,
          kDecompressionEnabled
              ? MessageList{"File Name: \xF0\x93\x82\x80",
                            "File Name: \xF0\x9F\x90\x9C",
                            "File Name: "
                            "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2"
                            "\x80\x8D"
                            "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6",
                            "File Name: \xF0\xA0\xAE\xB7",
                            "File Permission: Directory",
                            "File Permission: File",
                            "Decompressed compression unit 0 into 1024 bytes"}
              : MessageList{"File Name: \xF0\x93\x82\x80",
                            "File Name: \xF0\x9F\x90\x9C",
                            "File Permission: Directory",
                            "File Permission: File",
                            "Compressed attribute rejected: decompression is "
                            "not "
                            "compiled in."}}},
        {"corrupt_compressed_index_allocation",
         {kDecompressionEnabled,
          {"Cannot decompress compression unit 0",
           "LZNT1: back-reference before start of chunk.",
           "per compression unit", "Compressed size = "}}},
        {"compressed_index_allocation_comp_unit_size_out_of_range",
         {true, kDecompressionEnabled
                    ? MessageList{"Compression unit size is out of range.",
                                  "Attribute Parse error: 0x00A0"}
                    : MessageList{"Attribute Parse error: 0x00A0",
                                  "Compressed attribute rejected: "
                                  "decompression is not "
                                  "compiled in."}}},
        {"compressed_index_allocation_oversized_compression_unit",
         {true, kDecompressionEnabled
                    ? MessageList{"Compression unit size is implausibly large.",
                                  "Attribute Parse error: 0x00A0"}
                    : MessageList{"Attribute Parse error: 0x00A0",
                                  "Compressed attribute rejected: "
                                  "decompression is not "
                                  "compiled in."}}},
        {"compressed_index_allocation_misaligned_start_vcn",
         {true,
          kDecompressionEnabled
              ? MessageList{"Compressed attribute start VCN is not compression "
                            "unit aligned.",
                            "Attribute Parse error: 0x00A0"}
              : MessageList{"Attribute Parse error: 0x00A0",
                            "Compressed attribute rejected: decompression is "
                            "not "
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
         {true,
          {"Cannot calulate number of IndexBlocks",
           "Index Block: sub-node vcn out of bounds"}}},
        {"index_root_view_not_supported", {true, {"Index View not supported"}}},
        {"index_block_usn_mismatch",
         {true, {"Index Block parse error: Update Sequence Number"}}},
        {"index_block_entry_header_exceeds_bounds",
         {true, {"Index Block: index entry header exceeds block bounds"}}},
        {"index_root_entry_header_exceeds_bounds",
         {true,
          {"Index Root: index entry header exceeds attribute bounds",
           "Index Root attribute has a malformed index entry."}}},
        {"data_run_decode_error_size_byte",
         {true,
          {"DataRun decode error 1: 0x", "Data run is malformed.",
           "Attribute List parse error (ParseFileRecord)."}}},
        {"index_block_entry_exceeds_bounds",
         {true, {"Index Block: index entry exceeds block bounds"}}},
        {"data_run_decode_error_second",
         {true, {"DataRun decode error 2", "Data run LCN underflows."}}},
        {"data_run_vcn_exceeds_bound",
         {true,
          {"DataRun decode error: VCN exceeds bound",
           "Data run VCN exceeds the attribute's declared bound."}}},
        {"data_run_cluster_exceeds_bounds",
         {true,
          {"Cluster exceeds DataRun bounds",
           "TraverseSubEntries() recovery: orphan scan capped"}}},
        {"index_entry_no_filename_stream",
         {true, {"No Filename stream found"}}},
        {"index_entry_stream_smaller_than_expected",
         {true,
          {"Index Entry stream smaller than expected",
           "Index Root attribute has a malformed index entry."}}},
        {"index_entry_stream_exceeds_bounds",
         {true,
          {"Index Entry stream exceeds entry bounds",
           "Index Root attribute has a malformed index entry."}}},
        {"file_record_read_failure", {true, {"Cannot read file record 5"}}},
        {"file_record_invalid_magic", {true, {"Invalid file record"}}},
        {"file_record_usn_mismatch", {true, {"Update Sequence Number error"}}},
        {"boot_sector_read_failure", {true, {"Read boot sector error"}}},
        {"file_reader_read_failure", {true, {"Cannot read file at adress"}}},
        {"traverse_attrs_empty_callback",
         {true, {"TraverseAttrs() called with an empty callback"}}},
        {"file_record_unhandled_attribute",
         {true, {"Unhandled attribute: 0x0040"}}},
        {"bitmap_resident_data_read",
         {true, {"8 bytes of resident Bitmap data read"}}},
        {"full_cache_attribute_list_record_growth",
         {true,
          {"Attribute continuation VCNs are not contiguous from 0; "
           "leaving"}}},
        {"attr_type_slot_aliasing", {true, {"FileRecord Size is invalid"}}},
        {"full_cache_index_block_crosses_64kib_block",
         {true, {"clusters_per_index_block magnitude out of range"}}},
        {"invalid_header_common", {true, {"FileRecord Size is invalid"}}},
        {"compressed_index_allocation_encrypted",
         {true, {"FindStream() found the unnamed stream"}}},
        {"corrupt_mft_record_volume_ok", {true, {"Invalid file record"}}},
        {"efs_stream_too_large",
         {kEfsEnabled, {"$EFS stream is too large: 131072 bytes."}}},
        {"efs_stream_read_failure",
         {kEfsEnabled, {"Cannot read the $EFS stream."}}},
        {"data_flagged_compressed_and_encrypted",
         {true,
          {"A $DATA stream is flagged both compressed and encrypted; NTFS "
           "never combines them. Reading it undecrypted."}}},
        {"resident_data_flagged_encrypted",
         {true, {"A resident $DATA is flagged encrypted. Read as is."}}},
        {"efs_stream_malformed", {kEfsEnabled, {"Malformed $EFS stream."}}},
        {"standard_information_must_be_resident",
         {true, {"Standard Information attribute must be resident."}}},
        {"volume_name_must_be_resident",
         {true, {"Volume Name attribute must be resident."}}},
        {"volume_information_must_be_resident",
         {true, {"Volume Information attribute must be resident."}}},
        {"index_root_must_be_resident",
         {true, {"Index Root attribute must be resident."}}},
        {"index_allocation_must_be_non_resident",
         {true, {"Index Allocation attribute must be non-resident."}}},
        {"root_record_deleted_skips_parse_attrs",
         {true, {"ParseAttrs() skipped: file record 5 is deleted"}}},
        {"attribute_walk_no_end_marker",
         {true, {"Attribute walk ended without a terminating end marker."}}},
        {"index_root_entry_ab_match",
         {true, {"FindSubEntry() found entry in Index Root"}}},
        {"volume_name_resident_present", {true, {"NTFS volume name: TESTVOL"}}},
        {"attribute_list_invalid_attr_type",
         {true, {"Attribute List parse error (al_record.attr_type)."}}},
        {"attribute_list_extension_parse_attrs_fail",
         {true, {"Attribute List parse error (ParseAttrs)."}}},
        {"attribute_list_zero_record_size",
         {true, {"Attribute List with zero record size has endless loop."}}},
        {"attribute_list_record_size_too_small_on_root",
         {true,
          {"Attribute List: record_size 5 is smaller than the entry "
           "header 26 - stopping"}}},
        {"attribute_list_offset_mismatch_on_root",
         {true,
          {"Attribute List ended at offset 30 instead of its declared "
           "size 26."}}},
        {"index_root_entry_total_exceeds_declared_size",
         {true,
          {"Index Root: index entry total exceeds the attribute's "
           "declared entry size"}}},
        {"index_block_entry_total_exceeds_declared_size",
         {true,
          {"Index Block: index entry total exceeds the block's declared "
           "entry size"}}},
        {"index_block_subnode_vcn_overflow",
         {true, {"Index Block: sub-node vcn overflows byte offset"}}},
        {"mft_data_last_vcn_overflow",
         {true,
          {"$MFT DATA continuation's last VCN (18014398509481983) overflows a "
           "byte offset"}}},
};

// Runs one saved regression testcase and, if kExpectedErrorMessages has an
// entry for it, checks its output against that entry's expected messages.
void RunRegressionTestcase(std::string_view name)
{
  const fs::path exe(NTFS_FUZZER_AFL_EXE);
  REQUIRE(fs::exists(exe));

  const fs::path file = fs::path(NTFS_FUZZ_DATA_DIR) / name;
  REQUIRE(fs::exists(file));

  const ProcessOutput result = RunProcessCapturingOutput(
      exe, {L"--inject-read-failures", file.wstring()});
  CHECK(result.exit_code == 0);

  const auto it = kExpectedErrorMessages.find(name);
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
      CHECK_THAT(result.output,
                 Catch::Matchers::ContainsSubstring(std::string(message)));
    }
  }
}

}  // namespace

TEST_CASE("saved regression corpus is fully covered by kExpectedErrorMessages",
          "[fuzz][regression]")
{
  const std::vector<fs::path> files = ListRegressionTestcases();
  REQUIRE_FALSE(files.empty());

  for (const fs::path& file : files)
  {
    CHECK(kExpectedErrorMessages.contains(file.filename().string()));
  }
  CHECK(files.size() == kExpectedErrorMessages.size());
}

// Registers one ctest-visible TEST_CASE per saved regression testcase, so
// ctest can rerun a single failing input instead of the whole corpus.
#define NTFS_REGRESSION_TESTCASE(name)                               \
  TEST_CASE("NtfsFuzzerAfl regression: " name, "[fuzz][regression]") \
  {                                                                  \
    RunRegressionTestcase(name);                                     \
  }

NTFS_REGRESSION_TESTCASE("0724c913e1b2f0607bb5cd3ebfacb596db4458e9")
NTFS_REGRESSION_TESTCASE("65b60629c20b4730c35650dd68b6f87fe57c07a3")
NTFS_REGRESSION_TESTCASE("8fc085f7649f977b0ab5f67b5b9da055eebc56dd")
NTFS_REGRESSION_TESTCASE("9d6b29a12783a8d0595bf861671e5401493570b5")
NTFS_REGRESSION_TESTCASE("attr_name_exceeds_total_size")
NTFS_REGRESSION_TESTCASE("attr_offset_exceeds_record_size")
NTFS_REGRESSION_TESTCASE("attr_type_slot_aliasing")
NTFS_REGRESSION_TESTCASE("attribute_list_extension_parse_attrs_fail")
NTFS_REGRESSION_TESTCASE("attribute_list_extension_record_cycle")
NTFS_REGRESSION_TESTCASE("attribute_list_invalid_attr_type")
NTFS_REGRESSION_TESTCASE("attribute_list_multi_type_same_record")
NTFS_REGRESSION_TESTCASE("attribute_list_offset_mismatch_on_root")
NTFS_REGRESSION_TESTCASE("attribute_list_record_size_too_small_on_root")
NTFS_REGRESSION_TESTCASE("attribute_list_short_read")
NTFS_REGRESSION_TESTCASE("attribute_list_zero_record_size")
NTFS_REGRESSION_TESTCASE("attribute_walk_no_end_marker")
NTFS_REGRESSION_TESTCASE("bitmap_resident_data_read")
NTFS_REGRESSION_TESTCASE("boot_sector_read_failure")
NTFS_REGRESSION_TESTCASE("cluster_size_null")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation")
NTFS_REGRESSION_TESTCASE(
    "compressed_index_allocation_comp_unit_size_out_of_range")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_compressed_unit_bad_lcn")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_encrypted")
NTFS_REGRESSION_TESTCASE(
    "compressed_index_allocation_lznt1_backreference_exceeds_dest")
NTFS_REGRESSION_TESTCASE(
    "compressed_index_allocation_lznt1_chunk_exceeds_src_bounds")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_lznt1_chunk_over_4096")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_lznt1_invalid_signature")
NTFS_REGRESSION_TESTCASE(
    "compressed_index_allocation_lznt1_literal_exceeds_dest")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_lznt1_truncated_word")
NTFS_REGRESSION_TESTCASE(
    "compressed_index_allocation_lznt1_uncompressed_chunk_exceeds_dest")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_misaligned_start_vcn")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_missing_compressed_size")
NTFS_REGRESSION_TESTCASE(
    "compressed_index_allocation_oversized_compression_unit")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_real_after_hole")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_short_decompressed_unit")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_sparse_unit")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_stored_unit_bad_lcn")
NTFS_REGRESSION_TESTCASE("compressed_index_allocation_unmapped_unit")
NTFS_REGRESSION_TESTCASE("corrupt_compressed_index_allocation")
NTFS_REGRESSION_TESTCASE("corrupt_mft_record_volume_ok")
NTFS_REGRESSION_TESTCASE("data_flagged_compressed_and_encrypted")
NTFS_REGRESSION_TESTCASE("data_run_cluster_exceeds_bounds")
NTFS_REGRESSION_TESTCASE("data_run_decode_error_second")
NTFS_REGRESSION_TESTCASE("data_run_decode_error_size_byte")
NTFS_REGRESSION_TESTCASE("data_run_vcn_exceeds_bound")
NTFS_REGRESSION_TESTCASE("efs_stream_malformed")
NTFS_REGRESSION_TESTCASE("efs_stream_read_failure")
NTFS_REGRESSION_TESTCASE("efs_stream_too_large")
NTFS_REGRESSION_TESTCASE("f2a2482f50a933eeea4d1a506651884827c0952d")
NTFS_REGRESSION_TESTCASE("file_reader_read_failure")
NTFS_REGRESSION_TESTCASE("file_record_invalid_magic")
NTFS_REGRESSION_TESTCASE("file_record_read_failure")
NTFS_REGRESSION_TESTCASE("file_record_size_invalid")
NTFS_REGRESSION_TESTCASE("file_record_size_shift_overflow")
NTFS_REGRESSION_TESTCASE("file_record_size_too_big")
NTFS_REGRESSION_TESTCASE("file_record_unhandled_attribute")
NTFS_REGRESSION_TESTCASE("file_record_usn_mismatch")
NTFS_REGRESSION_TESTCASE("find_stream_named_data")
NTFS_REGRESSION_TESTCASE("fragmented_record_header_factory_throw")
NTFS_REGRESSION_TESTCASE("full_cache_attribute_list_record_growth")
NTFS_REGRESSION_TESTCASE("full_cache_index_block_crosses_64kib_block")
NTFS_REGRESSION_TESTCASE("gap_collation_subnode")
NTFS_REGRESSION_TESTCASE("index_alloc_block_count_incalculable")
NTFS_REGRESSION_TESTCASE("index_allocation_must_be_non_resident")
NTFS_REGRESSION_TESTCASE("index_block_chain_depth_limit")
NTFS_REGRESSION_TESTCASE("index_block_entry_exceeds_bounds")
NTFS_REGRESSION_TESTCASE("index_block_entry_header_exceeds_bounds")
NTFS_REGRESSION_TESTCASE("index_block_entry_total_exceeds_declared_size")
NTFS_REGRESSION_TESTCASE("index_block_magic_mismatch")
NTFS_REGRESSION_TESTCASE("index_block_offset_of_us_out_of_bounds")
NTFS_REGRESSION_TESTCASE("index_block_size_invalid")
NTFS_REGRESSION_TESTCASE("index_block_size_shift_overflow")
NTFS_REGRESSION_TESTCASE("index_block_subnode_vcn_overflow")
NTFS_REGRESSION_TESTCASE("index_block_usn_mismatch")
NTFS_REGRESSION_TESTCASE("index_entry_no_filename_stream")
NTFS_REGRESSION_TESTCASE("index_entry_stream_exceeds_bounds")
NTFS_REGRESSION_TESTCASE("index_entry_stream_smaller_than_expected")
NTFS_REGRESSION_TESTCASE("index_root_entry_ab_match")
NTFS_REGRESSION_TESTCASE("index_root_entry_header_exceeds_bounds")
NTFS_REGRESSION_TESTCASE("index_root_entry_total_exceeds_declared_size")
NTFS_REGRESSION_TESTCASE("index_root_must_be_resident")
NTFS_REGRESSION_TESTCASE("index_root_real_entry")
NTFS_REGRESSION_TESTCASE("index_root_view_not_supported")
NTFS_REGRESSION_TESTCASE("invalid_header_common")
NTFS_REGRESSION_TESTCASE("invalid_offset_of_us")
NTFS_REGRESSION_TESTCASE("mft_addr_narrowing_error")
NTFS_REGRESSION_TESTCASE("mft_data_last_vcn_overflow")
NTFS_REGRESSION_TESTCASE("mft_data_run_cluster_lcn_narrowing_error")
NTFS_REGRESSION_TESTCASE("resident_attr_body_exceeds_bounds")
NTFS_REGRESSION_TESTCASE("resident_attr_body_out_of_bounds")
NTFS_REGRESSION_TESTCASE("resident_data_flagged_encrypted")
NTFS_REGRESSION_TESTCASE("root_record_deleted_skips_parse_attrs")
NTFS_REGRESSION_TESTCASE("root_record_parse_failure")
NTFS_REGRESSION_TESTCASE("sector_size_too_small")
NTFS_REGRESSION_TESTCASE("standard_information_minimal_size")
NTFS_REGRESSION_TESTCASE("standard_information_must_be_resident")
NTFS_REGRESSION_TESTCASE("surrogate_pair_names")
NTFS_REGRESSION_TESTCASE("traverse_attrs_empty_callback")
NTFS_REGRESSION_TESTCASE("usn_array_exceeds_record_buffer")
NTFS_REGRESSION_TESTCASE("volume_information_minimal_size")
NTFS_REGRESSION_TESTCASE("volume_information_must_be_resident")
NTFS_REGRESSION_TESTCASE("volume_name_must_be_resident")
NTFS_REGRESSION_TESTCASE("volume_name_resident_present")

#undef NTFS_REGRESSION_TESTCASE
