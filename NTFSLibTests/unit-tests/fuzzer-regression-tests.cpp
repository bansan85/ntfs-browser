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
// invalid_header_common, corrupt_mft_record_volume_ok,
// full_cache_index_block_crosses_64kib_block,
// full_cache_attribute_list_record_growth - see the comments near the bottom
// of kExpectedErrorMessages for why those three have none).

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
        //
        // Regenerated (root record #5, boot sector + $Volume + $MFT + root,
        // concatenated in LoopingDiskReader's read order, same as the other
        // from-scratch corpus files): the original 74-byte AFL find no
        // longer reached this check at all, tripping "FileRecord Size is
        // invalid\n" - the sector-alignment bound 3d77eb8 added to
        // ParseBootSector() - before ParseAttrs() ever ran.
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
        // Root directory (#5) whose $INDEX_ALLOCATION's sole index block
        // declares offset_of_us = 0xFFFF - exercises the equivalent bound
        // AttrIndexAlloc<S>::ParseIndexBlock() now enforces via
        // IndexBlockUsOffsetInBounds() on the index block's own USN fixup
        // array (bug F4, see index-block-fixup-tests.cpp for the matching
        // unit test).
        {"index_block_offset_of_us_out_of_bounds",
         {"Index Block parse error: offset_of_us out of bounds"}},
        // clusters_per_file_record = 0xFF ("sz = -1") yields
        // file_record_size_ = 2, far too small to hold
        // FileRecordHeader::Data - exercises the bound
        // NtfsVolume<S>::ParseBootSector() now enforces on file_record_size_
        // (bug F6, see boot-sector-index-block-size-tests.cpp for the
        // matching unit test).
        {"file_record_size_invalid", {"FileRecord Size is invalid"}},
        // Same as above but clusters_per_index_block = 0xFF, yielding
        // index_block_size_ = 2 - exercises the equivalent bound on
        // index_block_size_ (bug F6, the check
        // boot-sector-index-block-size-tests.cpp's unit test targets).
        {"index_block_size_invalid", {"IndexBlock Size is invalid"}},
        // clusters_per_file_record = 0xE1 ("sz = -31") yields 1U << 31 ==
        // 0x80000000 (2 GiB): a well-defined shift (unlike sz = -128, which
        // really would be UB), but far too large a size for F6's checks
        // above to catch (0x80000000 is >= sizeof(FileRecordHeader::Data)
        // and a whole number of sectors). Exercises the bound
        // NtfsVolume<S>::ParseBootSector() now enforces on sz itself, before
        // the shift ever runs (bug F5, see
        // boot-sector-oversized-record-size-tests.cpp for the matching unit
        // test). Only the boot sector's own 512 bytes are needed - just like
        // file_record_size_invalid/index_block_size_invalid above, this
        // check trips in ParseBootSector() itself, before any FileRecord read
        // happens.
        {"file_record_size_shift_overflow",
         {"clusters_per_file_record magnitude out of range"}},
        // Same as above but clusters_per_index_block = 0xE1, yielding
        // index_block_size_ = 0x80000000 - exercises the equivalent bound on
        // clusters_per_index_block (bug F5).
        {"index_block_size_shift_overflow",
         {"clusters_per_index_block magnitude out of range"}},
        // clusters_per_file_record = 8 ("sz = 8", the largest magnitude F5's
        // own bound still allows) yields file_record_size_ = cluster_size_ *
        // 8 = 8192 - legal under every check that predates F9 (a whole
        // number of sectors, comfortably >= kMinFileRecordHeaderSize) but
        // twice FileRecordHeader::kMaxFileRecordSize (4096), the largest
        // size FileRecordHeader::Data::raw can actually hold. Exercises the
        // new upper-bound check NtfsVolume<S>::ParseBootSector() now
        // enforces on file_record_size_ (bug F9, see
        // file-record-header-size-tests.cpp for the matching unit tests on
        // FileRecordHeader itself). Only the boot sector's own 512 bytes are
        // needed - same recipe as file_record_size_shift_overflow above,
        // this check trips in ParseBootSector() itself, before any
        // FileRecord read happens.
        {"file_record_size_too_big",
         {"FileRecord Size exceeds the maximum supported file record size"}},
        {"sector_size_too_small", {"Sector Size must be at least 2 bytes"}},
        // Regenerated (root record #5, its own resident $ATTRIBUTE_LIST
        // naming record #6, whose own resident $ATTRIBUTE_LIST names #5
        // right back - boot sector + $Volume + $MFT + root + record #6,
        // concatenated in LoopingDiskReader's read order): the original raw
        // AFL find no longer reaches this message unmodified once the F10
        // fix (docs/bug-reports/2026-09-03-full-repo.md) changed
        // AttrResident<S>::ReadData()'s return value for every resident
        // $ATTRIBUTE_LIST read (this testcase's own $ATTRIBUTE_LIST body
        // size was not an exact multiple of sizeof(Attr::AttributeList),
        // so it now hits the new short-read stop below before ever reaching
        // the cycle). BuildFakeNtfsImageWithAttributeListCycle()
        // (fake-ntfs-image.h/.cpp) reproduces the intended two-record cycle
        // from scratch with a single, exactly-sized entry per record,
        // sidestepping that entirely - see attribute-list-tests.cpp for the
        // matching unit test.
        {"attribute_list_extension_record_cycle",
         {"already resolved in this chain, skipping"}},
        // Root record (#5, built from scratch the same way as
        // attr_name_exceeds_total_size - boot sector + $Volume + $MFT +
        // root, concatenated in LoopingDiskReader's read order): a single
        // resident $ATTRIBUTE_LIST whose real data size (50 bytes) is not
        // an exact multiple of sizeof(Attr::AttributeList) (40, at the time
        // this testcase was recorded - pre-F11) - regression fixture for
        // the AttrList<S>::AttrList() hardening added alongside F10
        // (docs/bug-reports/2026-09-03-full-repo.md): al_record is a single
        // stack variable reused across loop iterations, never reset between
        // them, so a short final read must stop the loop cleanly (now
        // observably, via this NTFS_TRACE2 call) instead of parsing a
        // "phantom" final entry built from a mix of freshly-read and stale
        // bytes. See attribute-list-tests.cpp for the matching unit test
        // (BuildFakeNtfsImageWithAttributeListShortRead()).
        //
        // Not regenerated for bug F11
        // (docs/bug-reports/2026-09-03-full-repo.md): its raw bytes already
        // encode a literal record_size of 40 for
        // entry 1 (read as a plain WORD field, unaffected by
        // MftSegmentReference's bitfield fix), so AttrList<S>::AttrList()'s
        // read loop still advances the offset from 0 to 40 exactly as
        // before, then requests Attr::kAttributeListEntryHeaderSize (26,
        // not sizeof(Attr::AttributeList) post-fix) bytes with only 10 left
        // (50 - 40) - the same short read as before, just now measured
        // against the real on-disk entry size instead of the padded
        // struct's sizeof(). Only the expected message text below changed,
        // from "...expected 40..." to "...expected 26...".
        {"attribute_list_short_read",
         {"Attribute List: ReadData returned 10 bytes, expected 26 - "
          "stopping"}},
        // ROOT's own $ATTRIBUTE_LIST has 3 entries: (INDEX_ROOT, record 6),
        // (INDEX_ALLOCATION, record 6), (INDEX_ROOT, record 6 again) - the
        // 3rd is a genuine duplicate of the 1st's (record, type) pair and
        // must be skipped with this message, while the 2nd (a DIFFERENT
        // type on the SAME record) must still resolve normally - exactly
        // the distinction F17's fix (dedupe by (record, attribute type),
        // not record alone) makes. Before that fix, entry 2 would also
        // have been wrongly skipped as "already resolved". See F17 in
        // docs/bug-reports/2026-09-03-full-repo.md.
        {"attribute_list_multi_type_same_record",
         {"Attribute List: record 6, type 0x0090 already resolved in this "
          "chain, skipping"}},
        // F8 catch #1/#7 (src/attr-non-resident.cpp,
        // AttrNonResident<S>::ReadClusters()): ROOT's $ATTRIBUTE_LIST
        // relocates $INDEX_ALLOCATION to record #16 (>= Enum::MftIdx::USER),
        // reachable only through volume_.mft_data_ ($MFT's own DATA
        // attribute) - patched to a single data run whose LCN makes
        // lcn * cluster_size overflow a LONGLONG. gsl::narrowing_error is
        // caught right at the throw site (not just runtime_error, since F8
        // widened this exact catch) and reported through ReadClusters()'s
        // normal std::optional channel; the resulting ParseFileRecord()
        // failure makes AttrList's ctor throw, which FileRecord::ParseAttr()
        // catches in turn (also widened by F8) - one fixture, two of F8's
        // seven touched catches. See F8 in
        // docs/bug-reports/2026-09-03-full-repo.md.
        {"mft_data_run_cluster_lcn_narrowing_error",
         {"Cannot read cluster with LCN", "narrowing_error",
          "Attribute Parse error: 0x0020"}},
        // F8 catch #6 (src/file-record.cpp, FileRecord<S>::ReadFileRecord()'s
        // fragmented-$MFT-path FileRecordHeader::Factory<S>() call) - this
        // try/catch did not exist at all before F8; any exception here used
        // to escape unguarded. Same $ATTRIBUTE_LIST -> record #16
        // mechanism as above, but $MFT's DATA run now points at a real,
        // in-bounds location holding record #16's own raw bytes: valid
        // magic, but offset_of_us (1024) is out of bounds, so
        // FileRecordHeader's ctor throws "Offset must be lower than 1024."
        // from inside this exact path.
        {"fragmented_record_header_factory_throw",
         {"Offset must be lower than 1024.", "Attribute Parse error: 0x0020"}},
        // F8 catch #7 (src/ntfs-volume.cpp, NtfsVolume<S>::ParseBootSector()'s
        // upfront mft_addr_ bound): lcn_mft = 2^53 makes mft_addr_ = 2^63 -
        // representable in the ULONGLONG it's stored in, but rejected
        // outright before $MFT/$Volume/root are ever read, since it would
        // later make FileRecord::ReadFileRecord()'s
        // gsl::narrow<LONGLONG>(mft_addr_ + ...) throw. Same fixture as
        // mft-addr-narrowing-tests.cpp's unit test.
        //
        // F8's other 5 touched catches: the 2 in ReadClusters() and the 1 in
        // ParseAttr() above are covered by this file and
        // mft_data_run_cluster_lcn_narrowing_error; the direct-allocation-path
        // FileRecordHeader::Factory<S>() call (widened runtime_error ->
        // exception, same as the fragmented-path one above) is already
        // exercised - on root record #5 itself - by invalid_offset_of_us and
        // usn_array_exceeds_record_buffer above. The remaining 2
        // (ReadClusters()'s `clusters` DWORD-narrow, and
        // ReadFileRecord()'s direct-allocation-path frAddr LONGLONG-narrow)
        // are dead code given other bounds already enforced elsewhere:
        // `clusters * cluster_size` can never reach 2^32 because it is
        // always <= index_block_size_, itself capped around 2.1e9 by how
        // clusters_per_index_block is decoded (a truncated signed byte);
        // and mft_addr_ + file_record_size_ * fileRef can never reach 2^63
        // because fileRef is bounded to 48 bits (MftSegmentReference /
        // Data::IndexEntry::mft_index) and mft_addr_ is itself already
        // capped at LLONG_MAX/2 by this very check - deliberately, to leave
        // headroom for that addition (see the comment in ParseBootSector()
        // right above the check). Both bounds are Strategy-independent, so
        // FULL_CACHE does not change this.
        {"mft_addr_narrowing_error", {"MFT address is invalid"}},
        // Root record (#5, built from scratch the same way as
        // resident_attr_body_out_of_bounds - boot sector + $Volume + $MFT +
        // root, concatenated in LoopingDiskReader's read order): a single
        // resident $DATA attribute whose name_offset/name_length reach 112
        // bytes in, past its own 28-byte total_size - exercises the bound
        // AttrBase<S>::GetAttrName() (src/attr-base.cpp) now enforces on
        // name_offset + 2*name_length against total_size (bug F2, see
        // attr-name-bounds-tests.cpp for the matching unit test). Reached
        // via FuzzOnce()'s FindStream() call (afl-main.cpp), added
        // specifically because GetAttrName() - like FindStream() itself -
        // used to be entirely unreached by this fuzzer.
        {"attr_name_exceeds_total_size",
         {"Attribute name exceeds attribute bounds."}},
        // Root record (#5, built the same way as attr_name_exceeds_total_size
        // just above - boot sector + $Volume + $MFT + root, concatenated in
        // LoopingDiskReader's read order) whose offset_of_attr (2000) is
        // patched well past its own 1024-byte extent - exercises the
        // HeaderCommon() half of bug F9: FileRecordHeader::HeaderCommon()
        // (src/data/file-record-header.cpp) bounds offset_of_attr against
        // this instance's own buffer_size_, and FileRecord::ParseAttrs()
        // (which calls HeaderCommon() to locate the first attribute, no
        // FindStream()/harness widening needed here, unlike F2) must reject
        // the whole record instead of reading past it. See
        // parse-attrs-total-size-tests.cpp for the matching unit test. Only
        // reproduces under NO_CACHE - like full_cache_index_block_crosses_
        // 64kib_block/full_cache_attribute_list_record_growth below,
        // FULL_CACHE's very different read-size pattern diverges into an
        // unrelated "Invalid file record" for this same input; only exit
        // code 0 is asserted for that pass.
        {"attr_offset_exceeds_record_size",
         {"Offset of attr must be within the file record buffer"}},
        // Boot sector + $Volume + $MFT + root, concatenated in
        // LoopingDiskReader's read order (same recipe as the other
        // from-scratch corpus files) - $Volume's (#3) VOLUME_INFORMATION
        // attribute is shrunk to exactly 12 bytes, the true on-disk size,
        // via BuildFakeNtfsImageWithMinimalVolumeInformation()
        // (fake-ntfs-image.h/.cpp). This is NOT a malformed/fuzzed input:
        // it is a well-formed volume, and the whole point is that it must
        // parse *successfully* ("NTFS volume version: 3.1" - the message
        // NtfsVolume<S>::Init(), src/ntfs-volume.cpp, prints unconditionally
        // on both strategies once $Volume's attribute is accepted). Without
        // #pragma pack(1) on Attr::VolumeInformation (src/attr/
        // volume-information.h), alignof(ULONGLONG) pads sizeof() up to 16,
        // so AttrVolInfo's ctor (src/attr-vol-info.cpp) rejected this
        // real-size (12-byte) attribute with "Volume Information attribute
        // smaller than expected." instead - silently breaking every real
        // volume open, while every existing corpus/fixture kept passing
        // because they all size their own $Volume attribute off that same
        // (possibly inflated) sizeof(). See attr-vol-info-size-tests.cpp for
        // the matching unit test.
        {"volume_information_minimal_size", {"NTFS volume version: 3.1"}},
};

// No new corpus entry was added for bug F11 (docs/bug-reports/2026-09-03-
// full-repo.md, Attr::MftSegmentReference's non-bitfield WORD member and
// AttrList<S>::AttrList()'s sizeof(Attr::AttributeList)-sized read loop).
// The fix doesn't add a new NTFS_TRACE call of its own - it changes
// attribute_list_short_read's *existing* F10-hardening message above
// instead (the read width the loop compares against is now
// Attr::kAttributeListEntryHeaderSize (26), not sizeof(Attr::AttributeList)
// (40 pre-fix), so that testcase's captured message now reads "...expected
// 26..." rather than "...expected 40...") - already exactly the "natural,
// real, testable message tied to this fix's own effects" this file's own
// convention asks to check for first. F11's other, more interesting
// consequence - a densely-packed $ATTRIBUTE_LIST's final entry being
// silently dropped instead of resolved - is covered by
// attribute-list-tests.cpp's own dedicated unit test
// (BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory()), which
// checks the resolved attribute's actual content (GetDataSize(),
// FindSubEntry()) rather than merely a trace message's presence;
// reproducing that same assertion strength as a NtfsFuzzerAfl corpus entry
// would need this file to inspect resolved attribute data the way the
// fuzzer harness does not, so it would add scaffolding without adding real
// coverage beyond what that unit test (and the exit-code/message checks
// already in place) provide.

// The three fixtures below deliberately have no kExpectedErrorMessages
// entry - only the exit-code check above applies to them.
//
// corrupt_mft_record_volume_ok - regression fixture for F14
// (docs/bug-reports/2026-09-03-full-repo.md): NtfsVolume<S>::Init() used to
// set volume_ok_ = true right after $Volume parsed, before $MFT's own
// record was even read - if that failed (as it does here: $MFT is
// zero-filled), IsVolumeOK() would wrongly report true with mft_data_
// still null. The fix doesn't add a distinct NTFS_TRACE message of its
// own - GetRecordsCount(), the function that used to dereference that null
// pointer, is never called by this fuzzer at all - so this fixture only
// proves the fixed code path (Init() returning early without ever setting
// volume_ok_) runs cleanly for this input, same as
// mft-parse-failure-volume-ok-tests.cpp's unit test (which does check
// IsVolumeOK()/GetRecordsCount() directly).
//
// full_cache_index_block_crosses_64kib_block and
// full_cache_attribute_list_record_growth - F19 and N3
// (docs/bug-reports/2026-09-03-full-repo.md) only manifest under
// Strategy::FULL_CACHE - NtfsFuzzerAfl used to hardcode Strategy::NO_CACHE
// and could never reach either, so afl-main.cpp now runs every testcase
// through both strategies. Neither fixture was recorded to make sense
// under NO_CACHE too (it replays the same bytes under both strategies from
// the same starting position, so NO_CACHE's very different read-size
// pattern diverges quickly into an unrelated, harmless parse failure -
// "Invalid file record" in these two cases; only exit code 0 is asserted).
//
// full_cache_index_block_crosses_64kib_block: ROOT's $INDEX_ROOT/
// $INDEX_ALLOCATION describe one 128KiB index block via a single data run
// starting at a non-block-aligned LCN, so reading it crosses more than one
// of FileReader<FULL_CACHE>::Read()'s 64KiB cache blocks in one call - the
// exact case that used to only be guarded by an assert() (compiled out in
// Release) and, if it slipped through anyway, returned a span reaching
// past the single 64KiB slice actually read. "Successfully read 128
// clusters from LCN 24" in the captured output confirms the crossing read
// completed instead of asserting/crashing.
//
// full_cache_attribute_list_record_growth: ROOT's $ATTRIBUTE_LIST
// relocates $INDEX_ALLOCATION into 4 distinct extension records under
// FULL_CACHE, growing AttrList's file_record_list_ repeatedly - the exact
// non-forged shape N3 describes (a heavily fragmented directory's
// $INDEX_ALLOCATION relocated across several extension records).
// file_record_list_ is a std::list (the N3 fix), so this no longer
// relocates already-constructed FileRecords/dangling attribute references
// on growth; this fuzzer doesn't inspect GetDataSize() values the way
// attribute-list-tests.cpp's unit test does, so a clean exit here is the
// only signal available - a real regression back to std::vector would
// need that unit test to catch it, not this file.
//
// Two of bug F9's own messages have no corpus file at all, not even one
// without a kExpectedErrorMessages entry: FileRecordHeader's ctor
// (src/data/file-record-header.cpp) rejects buffer.size() outside
// [kMinFileRecordHeaderSize, kMaxFileRecordSize] with "Buffer size of
// FileRecordHeader is smaller than the minimum file record header size."/
// "...exceeds the maximum supported file record size.", but both
// FileRecord<S>::ReadFileRecord() call sites (src/file-record.cpp) only ever
// construct a FileRecordHeader from record_buffer_, which is always resized
// to exactly volume_.GetFileRecordSize() - a value NtfsVolume<S>::
// ParseBootSector() already validated against that very same range before
// Init() (and so ReadFileRecord()) can ever run. So through the real
// NtfsVolume/FileRecord pipeline both fuzzers drive, file_record_size_ can
// never disagree with FileRecordHeader's own ctor bound by the time it gets
// there - this ctor-level check is dead code from a fuzzing perspective,
// exactly like the two F8 catches documented above it. Real coverage lives
// in file-record-header-size-tests.cpp instead, which calls
// FileRecordHeader::Factory<S>() directly with buffer sizes no volume-level
// check ever gates.
//
// Bug F15 (FileRecord<S>::IsDeleted()/IsDirectory() dereferencing an empty
// file_record_ optional, docs/bug-reports/2026-09-03-full-repo.md) has no
// corpus entry either, and for a similar "unreachable from here" reason,
// though the mechanism is different from the two above: it isn't gated out
// by an earlier bounds check that always fires first - it's simply never
// exercised at all, from either fuzzer, because neither main.cpp's
// FuzzOnce() nor afl-main.cpp's FuzzOnce() ever calls IsDeleted() or
// IsDirectory() in the first place (both stop at TraverseSubEntries()/
// FindStream()), and both bail out via an early "return;" the moment
// ParseFileRecord()/ParseAttrs() fails, before reaching any call that
// could. This bug is purely a caller-discipline API contract issue - a
// public accessor called before/without a successful parse - not something
// any sequence of on-disk bytes fed through the fuzz harnesses' fixed call
// sequence could ever trigger. Real coverage lives in
// file-record-unparsed-flags-tests.cpp instead, which constructs a
// FileRecord<S> and calls IsDeleted()/IsDirectory() directly without ever
// calling ParseFileRecord() on it.

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
