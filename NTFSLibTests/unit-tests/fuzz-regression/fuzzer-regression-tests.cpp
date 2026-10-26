#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "fuzz-core.h"
#include "fuzzer-expected-messages.h"
#include "looping-disk-reader.h"
#include "ntfs-browser/win-types.h"
#include "test-log-sink.h"

using NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog;
using NTFS_BROWSER_NAMESPACE::Tests::FuzzRegression::expected_error_messages;

namespace Fs = std::filesystem;

namespace {

// Lists the saved AFL testcases under NTFS_FUZZ_DATA_DIR, sorted.
std::vector<Fs::path> ListRegressionTestcases() {
  std::vector<Fs::path> files;
  for (const auto& entry :
       Fs::directory_iterator(Fs::path(NTFS_FUZZ_DATA_DIR))) {
    if (entry.is_regular_file()) {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  return files;
}

// The boot sector fields FuzzTestcase() patches for a testcase: all of them,
// except the one whose check the testcase tests.
NtfsFuzz::BootSectorPatch PatchFor(std::string_view name) {
  return {.marker = name != "boot_sector_missing_aa55_marker",
          .total_sectors = name != "boot_sector_total_sectors_zero" &&
                           name != "boot_sector_mft_beyond_total_sectors",
          .reserved_fields = name != "boot_sector_reserved_sectors_nonzero" &&
                             name != "boot_sector_fat_count_nonzero" &&
                             name != "boot_sector_large_sectors_nonzero"};
}

// Runs one saved regression testcase in-process and, if
// expected_error_messages has an entry for it, checks the messages it logged
// against that entry's expected messages.
void RunRegressionTestcase(std::string_view name) {
  const Fs::path file = Fs::path(NTFS_FUZZ_DATA_DIR) / name;
  REQUIRE(Fs::exists(file));

  // Same as NtfsFuzzerAfl: an empty or unreadable testcase runs nothing.
  std::optional<std::vector<BYTE>> data =
      NtfsFuzz::LoopingDiskReader::LoadFile(file);
  if (!data) {
    return;
  }

  // Drops anything logged by an earlier case before this one runs.
  (void)TakeCapturedLog();
  // A library exception escaping FuzzTestcase() is a failure here, as a
  // crash would be in the old subprocess run.
  CHECK_NOTHROW(NtfsFuzz::FuzzTestcase(*data, true, PatchFor(name)));
  const std::string output = TakeCapturedLog();

  const auto* const iterator = expected_error_messages.find(name);
  if (iterator != expected_error_messages.end() &&
      iterator->second.check_expected_messages) {
    INFO("captured log:\n" << output);
    for (const std::string_view message : iterator->second.messages) {
      // Trailing array slots past this testcase's own messages are
      // empty padding; stop there instead of matching real content.
      if (message.empty()) {
        break;
      }
      CHECK_THAT(output,
                 Catch::Matchers::ContainsSubstring(std::string(message)));
    }
  }
}

}  // namespace

TEST_CASE("saved regression corpus is fully covered by expected_error_messages",
          "[fuzz][regression]") {
  const std::vector<Fs::path> files = ListRegressionTestcases();
  REQUIRE_FALSE(files.empty());

  for (const Fs::path& file : files) {
    CHECK(expected_error_messages.contains(file.filename().string()));
  }
  CHECK(files.size() == expected_error_messages.size());
}

// Registers one ctest-visible TEST_CASE per saved regression testcase, so
// ctest can rerun a single failing input instead of the whole corpus.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): TEST_CASE needs a literal.
#define NTFS_REGRESSION_TESTCASE(name)                                 \
  TEST_CASE("NtfsFuzzerAfl regression: " name, "[fuzz][regression]") { \
    RunRegressionTestcase(name);                                       \
  }

NTFS_REGRESSION_TESTCASE("0724c913e1b2f0607bb5cd3ebfacb596db4458e9")
NTFS_REGRESSION_TESTCASE("65b60629c20b4730c35650dd68b6f87fe57c07a3")
NTFS_REGRESSION_TESTCASE("8fc085f7649f977b0ab5f67b5b9da055eebc56dd")
NTFS_REGRESSION_TESTCASE("9d6b29a12783a8d0595bf861671e5401493570b5")
NTFS_REGRESSION_TESTCASE("alloc_bitmap_cut_run_list")
NTFS_REGRESSION_TESTCASE("alloc_bitmap_forged_size")
NTFS_REGRESSION_TESTCASE("alloc_bitmap_fragmented")
NTFS_REGRESSION_TESTCASE("alloc_bitmap_huge_volume_claim")
NTFS_REGRESSION_TESTCASE("alloc_bitmap_short_resident")
NTFS_REGRESSION_TESTCASE("alloc_bitmap_short_with_bad")
NTFS_REGRESSION_TESTCASE("alloc_scan_deleted_batch")
NTFS_REGRESSION_TESTCASE("alloc_scan_live_stream")
NTFS_REGRESSION_TESTCASE("bad_clusters_conflicting_flags")
NTFS_REGRESSION_TESTCASE("bad_clusters_cut_runs")
NTFS_REGRESSION_TESTCASE("bad_clusters_huge_count")
NTFS_REGRESSION_TESTCASE("bad_clusters_marked")
NTFS_REGRESSION_TESTCASE("bad_clusters_no_stream")
NTFS_REGRESSION_TESTCASE("bad_clusters_odd_flags")
NTFS_REGRESSION_TESTCASE("bad_clusters_out_of_order")
NTFS_REGRESSION_TESTCASE("bad_clusters_overlap")
NTFS_REGRESSION_TESTCASE("bad_clusters_past_volume")
NTFS_REGRESSION_TESTCASE("attr_list_nonres_entry_past_end")
NTFS_REGRESSION_TESTCASE("attr_name_exceeds_total_size")
NTFS_REGRESSION_TESTCASE("attr_offset_exceeds_record_size")
NTFS_REGRESSION_TESTCASE("attr_type_slot_aliasing")
NTFS_REGRESSION_TESTCASE("attribute_list_extension_foreign_record")
NTFS_REGRESSION_TESTCASE("attribute_list_extension_parse_attrs_fail")
NTFS_REGRESSION_TESTCASE("attribute_list_extension_record_cycle")
NTFS_REGRESSION_TESTCASE("attribute_list_invalid_attr_type")
NTFS_REGRESSION_TESTCASE("attribute_list_multi_cluster")
NTFS_REGRESSION_TESTCASE("attribute_list_multi_type_same_record")
NTFS_REGRESSION_TESTCASE("attribute_list_offset_mismatch_on_root")
NTFS_REGRESSION_TESTCASE("attribute_list_overwritten_cut_entry")
NTFS_REGRESSION_TESTCASE("attribute_list_overwritten_invalid_type")
NTFS_REGRESSION_TESTCASE("attribute_list_record_size_too_small_on_root")
NTFS_REGRESSION_TESTCASE("attribute_list_scan_both_sides")
NTFS_REGRESSION_TESTCASE("attribute_list_scan_foreign_extension")
NTFS_REGRESSION_TESTCASE("attribute_list_scan_non_resident")
NTFS_REGRESSION_TESTCASE("attribute_list_short_read")
NTFS_REGRESSION_TESTCASE("attribute_list_split_attribute")
NTFS_REGRESSION_TESTCASE("attribute_list_zero_record_size")
NTFS_REGRESSION_TESTCASE("attribute_walk_no_end_marker")
NTFS_REGRESSION_TESTCASE("cluster_state_compressed_map")
NTFS_REGRESSION_TESTCASE("cluster_state_cut_run_lists")
NTFS_REGRESSION_TESTCASE("cluster_state_extension_claims")
NTFS_REGRESSION_TESTCASE("cluster_state_freed_claims")
NTFS_REGRESSION_TESTCASE("cluster_state_no_bitmap")
NTFS_REGRESSION_TESTCASE("cluster_state_streams")
NTFS_REGRESSION_TESTCASE("bitmap_multi_cluster")
NTFS_REGRESSION_TESTCASE("bitmap_nonres_sparse_runs")
NTFS_REGRESSION_TESTCASE("bitmap_resident_data_read")
NTFS_REGRESSION_TESTCASE("boot_sector_mft_beyond_total_sectors")
NTFS_REGRESSION_TESTCASE("boot_sector_fat_count_nonzero")
NTFS_REGRESSION_TESTCASE("boot_sector_backup_fallback")
NTFS_REGRESSION_TESTCASE("boot_sector_large_sectors_nonzero")
NTFS_REGRESSION_TESTCASE("boot_sector_missing_aa55_marker")
NTFS_REGRESSION_TESTCASE("boot_sector_read_failure")
NTFS_REGRESSION_TESTCASE("boot_sector_reserved_sectors_nonzero")
NTFS_REGRESSION_TESTCASE("boot_sector_sector_size_above_range")
NTFS_REGRESSION_TESTCASE("boot_sector_sector_size_not_power_of_two")
NTFS_REGRESSION_TESTCASE("boot_sector_sector_size_out_of_range")
NTFS_REGRESSION_TESTCASE("boot_sector_sectors_per_cluster_not_power_of_two")
NTFS_REGRESSION_TESTCASE("boot_sector_total_sectors_zero")
NTFS_REGRESSION_TESTCASE("boot_sectors_per_cluster_shift_out_of_range")
NTFS_REGRESSION_TESTCASE("cluster_size_null")
NTFS_REGRESSION_TESTCASE("compressed_data_read")
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
NTFS_REGRESSION_TESTCASE("compressed_overrun_and_lznt1_chunk_overflow")
NTFS_REGRESSION_TESTCASE("compressed_split_runs_leave_gap")
NTFS_REGRESSION_TESTCASE("corrupt_compressed_index_allocation")
NTFS_REGRESSION_TESTCASE("corrupt_mft_record_volume_ok")
NTFS_REGRESSION_TESTCASE("data_flagged_compressed_and_encrypted")
NTFS_REGRESSION_TESTCASE("data_run_cluster_exceeds_bounds")
NTFS_REGRESSION_TESTCASE("data_run_decode_error_second")
NTFS_REGRESSION_TESTCASE("data_run_decode_error_size_byte")
NTFS_REGRESSION_TESTCASE("data_run_lcn_product_wraps")
NTFS_REGRESSION_TESTCASE("data_run_lcn_sum_overflows")
NTFS_REGRESSION_TESTCASE("data_run_vcn_exceeds_bound")
NTFS_REGRESSION_TESTCASE("efs_decrypt_3des_no_cache")
NTFS_REGRESSION_TESTCASE("efs_decrypt_aes128_no_cache")
NTFS_REGRESSION_TESTCASE("efs_decrypt_aes192_no_cache")
NTFS_REGRESSION_TESTCASE("efs_decrypt_aes256_no_cache")
NTFS_REGRESSION_TESTCASE("efs_decrypt_desx_full_cache")
NTFS_REGRESSION_TESTCASE("efs_encrypted_read_unaligned_cluster")
NTFS_REGRESSION_TESTCASE("efs_stream_malformed")
NTFS_REGRESSION_TESTCASE("efs_stream_malformed_fields")
NTFS_REGRESSION_TESTCASE("efs_stream_read_failure")
NTFS_REGRESSION_TESTCASE("efs_stream_too_large")
NTFS_REGRESSION_TESTCASE("efs_zero_size_stream")
NTFS_REGRESSION_TESTCASE("f2a2482f50a933eeea4d1a506651884827c0952d")
NTFS_REGRESSION_TESTCASE("file_reader_read_failure")
NTFS_REGRESSION_TESTCASE("file_record_invalid_magic")
NTFS_REGRESSION_TESTCASE("file_record_read_failure")
NTFS_REGRESSION_TESTCASE("file_record_size_invalid")
NTFS_REGRESSION_TESTCASE("file_record_size_shift_overflow")
NTFS_REGRESSION_TESTCASE("file_record_size_too_big")
NTFS_REGRESSION_TESTCASE("file_record_unhandled_attribute")
NTFS_REGRESSION_TESTCASE("file_record_usn_mismatch")
NTFS_REGRESSION_TESTCASE("filerecord_efs_stream_too_large")
NTFS_REGRESSION_TESTCASE("filerecord_filename_bad_lengths")
NTFS_REGRESSION_TESTCASE("filerecord_index_alloc_missing")
NTFS_REGRESSION_TESTCASE("filerecord_index_nested_subnode_descent")
NTFS_REGRESSION_TESTCASE("filerecord_index_nested_subnode_entry")
NTFS_REGRESSION_TESTCASE("filerecord_index_subnode_self_loop")
NTFS_REGRESSION_TESTCASE("filerecord_walk_runs_past_record_end")
NTFS_REGRESSION_TESTCASE("find_stream_named_data")
NTFS_REGRESSION_TESTCASE("fragmented_record_header_factory_throw")
NTFS_REGRESSION_TESTCASE("full_cache_attribute_list_record_growth")
NTFS_REGRESSION_TESTCASE("full_cache_index_block_crosses_64kib_block")
NTFS_REGRESSION_TESTCASE("gap_collation_subnode")
NTFS_REGRESSION_TESTCASE("index_alloc_block_count_incalculable")
NTFS_REGRESSION_TESTCASE("index_allocation_must_be_non_resident")
NTFS_REGRESSION_TESTCASE("index_allocation_split_runs")
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
NTFS_REGRESSION_TESTCASE("index_orphan_scan_huge_block_count")
NTFS_REGRESSION_TESTCASE("index_root_entry_ab_match")
NTFS_REGRESSION_TESTCASE("index_root_entry_header_exceeds_bounds")
NTFS_REGRESSION_TESTCASE("index_root_entry_total_exceeds_declared_size")
NTFS_REGRESSION_TESTCASE("index_root_must_be_resident")
NTFS_REGRESSION_TESTCASE("index_root_real_entry")
NTFS_REGRESSION_TESTCASE("index_root_view_not_supported")
NTFS_REGRESSION_TESTCASE("invalid_header_common")
NTFS_REGRESSION_TESTCASE("invalid_offset_of_us")
NTFS_REGRESSION_TESTCASE("mft_addr_narrowing_error")
NTFS_REGRESSION_TESTCASE("mft_attribute_list_extension_defects")
NTFS_REGRESSION_TESTCASE("mft_bitmap_lying")
NTFS_REGRESSION_TESTCASE("mft_bitmap_non_resident")
NTFS_REGRESSION_TESTCASE("mft_bitmap_resident")
NTFS_REGRESSION_TESTCASE("mft_bitmap_too_short")
NTFS_REGRESSION_TESTCASE("mft_data_extent_chain")
NTFS_REGRESSION_TESTCASE("mft_data_last_vcn_overflow")
NTFS_REGRESSION_TESTCASE("mft_data_merged_span_overflow")
NTFS_REGRESSION_TESTCASE("mft_data_run_cluster_lcn_narrowing_error")
NTFS_REGRESSION_TESTCASE("mft_data_without_base_extent")
NTFS_REGRESSION_TESTCASE("mft_huge_scan_stops_at_progress")
NTFS_REGRESSION_TESTCASE("mft_mirror_same_as_mft")
NTFS_REGRESSION_TESTCASE("mft_scan_broken_sectors")
NTFS_REGRESSION_TESTCASE("mft_scan_fragmented")
NTFS_REGRESSION_TESTCASE("mft_record0_corrupt_mirror_intact")
NTFS_REGRESSION_TESTCASE("mft_tree_full_cache")
NTFS_REGRESSION_TESTCASE("mft_tree_huge_real_size")
NTFS_REGRESSION_TESTCASE("mft_tree_names_damaged_record_and_duplicate_link")
NTFS_REGRESSION_TESTCASE("names_filename_zero_length_name")
NTFS_REGRESSION_TESTCASE("names_index_block_subnode_vcn_too_small")
NTFS_REGRESSION_TESTCASE("names_upcase_fold_surrogates_and_utf8")
NTFS_REGRESSION_TESTCASE("index_slack_damaged_blocks")
NTFS_REGRESSION_TESTCASE("index_slack_dense_names")
NTFS_REGRESSION_TESTCASE("index_slack_name_runs_past_block")
NTFS_REGRESSION_TESTCASE("index_slack_repeated_names")
NTFS_REGRESSION_TESTCASE("record_slack_dense_names")
NTFS_REGRESSION_TESTCASE("record_slack_live_names")
NTFS_REGRESSION_TESTCASE("record_slack_name_runs_past_record")
NTFS_REGRESSION_TESTCASE("record_slack_name_units")
NTFS_REGRESSION_TESTCASE("record_slack_rejected_fields")
NTFS_REGRESSION_TESTCASE("record_slack_repeated_names")
NTFS_REGRESSION_TESTCASE("record_slack_stale_terminators")
NTFS_REGRESSION_TESTCASE("resident_attr_body_exceeds_bounds")
NTFS_REGRESSION_TESTCASE("resident_attr_body_out_of_bounds")
NTFS_REGRESSION_TESTCASE("resident_data_flagged_encrypted")
NTFS_REGRESSION_TESTCASE("run_list_cut_after_two_runs")
NTFS_REGRESSION_TESTCASE("run_list_empty_run")
NTFS_REGRESSION_TESTCASE("run_list_overflowing_run")
NTFS_REGRESSION_TESTCASE("root_record_deleted_skips_parse_attrs")
NTFS_REGRESSION_TESTCASE("root_record_parse_failure")
NTFS_REGRESSION_TESTCASE("scan_threads_waiting_recovering")
NTFS_REGRESSION_TESTCASE("scan_threads_waiting_strict")
NTFS_REGRESSION_TESTCASE("sector_size_too_small")
NTFS_REGRESSION_TESTCASE("standard_information_minimal_size")
NTFS_REGRESSION_TESTCASE("standard_information_must_be_resident")
NTFS_REGRESSION_TESTCASE("surrogate_pair_names")
NTFS_REGRESSION_TESTCASE("traverse_attrs_empty_callback")
NTFS_REGRESSION_TESTCASE("upcase_run_lcn_at_address_space_limit")
NTFS_REGRESSION_TESTCASE("upcase_table_read_crosses_64k_blocks")
NTFS_REGRESSION_TESTCASE("usn_array_exceeds_record_buffer")
NTFS_REGRESSION_TESTCASE("usn_journal_adjacent_stretches")
NTFS_REGRESSION_TESTCASE("usn_journal_anchor_mismatch")
NTFS_REGRESSION_TESTCASE("usn_journal_forged_length")
NTFS_REGRESSION_TESTCASE("usn_journal_mixed_versions")
NTFS_REGRESSION_TESTCASE("usn_journal_name_past_record")
NTFS_REGRESSION_TESTCASE("usn_journal_sparse_head")
NTFS_REGRESSION_TESTCASE("usn_journal_truncated_tail")
NTFS_REGRESSION_TESTCASE("usn_journal_v4_record")
NTFS_REGRESSION_TESTCASE("usn_journal_zero_length")
NTFS_REGRESSION_TESTCASE("usn_loader_cut_run_list")
NTFS_REGRESSION_TESTCASE("usn_loader_no_extend_entry")
NTFS_REGRESSION_TESTCASE("usn_loader_no_j_stream")
NTFS_REGRESSION_TESTCASE("usn_loader_no_journal_record")
NTFS_REGRESSION_TESTCASE("usn_scanner_damaged_names")
NTFS_REGRESSION_TESTCASE("usn_tree_annotated")
NTFS_REGRESSION_TESTCASE("usn_tree_journal_only_paths")
NTFS_REGRESSION_TESTCASE("volume_information_minimal_size")
NTFS_REGRESSION_TESTCASE("volume_dirty_flag_set")
NTFS_REGRESSION_TESTCASE("volume_information_must_be_resident")
NTFS_REGRESSION_TESTCASE("volume_information_version_below_three")
NTFS_REGRESSION_TESTCASE("volume_name_must_be_resident")
NTFS_REGRESSION_TESTCASE("volume_name_resident_present")
NTFS_REGRESSION_TESTCASE("volume_record_corrupt_mirror_intact")

#undef NTFS_REGRESSION_TESTCASE
