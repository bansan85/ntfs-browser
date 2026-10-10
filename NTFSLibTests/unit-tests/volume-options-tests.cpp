#include <ntfs-browser/win-types.h>

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "attr/index-root.h"
#include "attr/resident.h"
#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

namespace NtfsBrowser {}  // namespace NtfsBrowser

using Catch::Matchers::ContainsSubstring;
using NtfsBrowser::Attr::AttrBase;
namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Attr::AttrIndexRoot;
using NtfsBrowser::Attr::AttrResidentFullCache;
using NtfsBrowser::Attr::AttrResidentNoCache;
namespace Mft = NtfsBrowser::Mft;
using NtfsBrowserTests::attr_name_exceeds_total_size_record_idx;
using NtfsBrowserTests::bad_index_block_first_name;
using NtfsBrowserTests::bad_index_block_good_name;
using NtfsBrowserTests::BuildFakeNtfsImage;
using NtfsBrowserTests::BuildFakeNtfsImageWithAttrNameExceedsTotalSize;
using NtfsBrowserTests::BuildFakeNtfsImageWithBadDataRun;
using NtfsBrowserTests::BuildFakeNtfsImageWithBadIndexBlockEntry;
using NtfsBrowserTests::BuildFakeNtfsImageWithDeletedVolumeRecord;
using NtfsBrowserTests::BuildFakeNtfsImageWithHugeOrphanScanBlockCount;
using NtfsBrowserTests::BuildFakeNtfsImageWithMalformedIndexEntryFilename;
using NtfsBrowserTests::BuildFakeNtfsImageWithMftTree;
using NtfsBrowserTests::BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock;
using NtfsBrowserTests::BuildFakeNtfsImageWithNoEndMarker;
using NtfsBrowserTests::BuildFakeNtfsImageWithResidentEncryptedData;
using NtfsBrowserTests::malformed_index_entry_mft_ref;
using NtfsBrowserTests::MemoryDiskReader;
using NtfsBrowserTests::mft_tree_deleted_file_idx;
using NtfsBrowserTests::multi_cluster_orphan_name;
using NtfsBrowserTests::multi_cluster_reachable_name;
using NtfsBrowserTests::orphaned_block_orphan_name;
using NtfsBrowserTests::orphaned_block_reachable_name;
using NtfsBrowserTests::TakeCapturedLog;

namespace {

// VolumeOptions{} default-constructed: both flags off.
template <Cache::Strategy S>
void RunDefaultsAreBothOff() {
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());
  CHECK_FALSE(volume.GetOptions().include_deleted);
  CHECK_FALSE(volume.GetOptions().recover_errors);
}

// GetOptions() reflects exactly what the constructor was given.
template <Cache::Strategy S>
void RunGetOptionsReflectsConstructorArgument() {
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImage()),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetOptions().include_deleted);
  CHECK(volume.GetOptions().recover_errors);
}

// Decision 1: include_deleted off also gates a freed record's content.
// ParseFileRecord() still reads its header (IsDeleted() works), but
// ParseAttrs() exposes nothing unless include_deleted is on.
template <Cache::Strategy S>
void RunDeletedRecordContentGating() {
  {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithMftTree()));
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(mft_tree_deleted_file_idx));
    CHECK(record.IsDeleted());
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::FileName).empty());
  }
  {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithMftTree()),
        VolumeOptions{.include_deleted = true});
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(mft_tree_deleted_file_idx));
    CHECK(record.IsDeleted());
    CHECK(record.ParseAttrs());
    CHECK_FALSE(record.GetAttr(Attr::Type::FileName).empty());
  }
}

// A salvageable condition (here, a masked-in attribute's name exceeding its
// own total_size) logs at Warn when strict and Info when recovering, with
// the same text either way.
template <Cache::Strategy S>
void RunSalvageableConditionLogLevel() {
  const std::shared_ptr<spdlog::logger> logger =
      spdlog::get(std::string(NtfsBrowser::Log::logger_name));
  REQUIRE(logger);

  const auto capture_for = [&](const VolumeOptions& options) {
    std::ostringstream out;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(out);
    sink->set_pattern("%l %v");
    logger->sinks().push_back(sink);

    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(
            BuildFakeNtfsImageWithAttrNameExceedsTotalSize()),
        options);
    REQUIRE(volume.IsVolumeOK());
    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(attr_name_exceeds_total_size_record_idx));
    (void)record.ParseAttrs();

    logger->sinks().pop_back();
    return out.str();
  };

  const std::string strict = capture_for({});
  CHECK_THAT(strict,
             ContainsSubstring("warning Attribute name exceeds attribute "
                               "bounds."));

  const std::string recovering =
      capture_for(VolumeOptions{.recover_errors = true});
  CHECK_THAT(recovering,
             ContainsSubstring("info Attribute name exceeds attribute "
                               "bounds."));
  CHECK_THAT(recovering,
             !ContainsSubstring("warning Attribute name exceeds attribute "
                                "bounds."));
}

// The orphan scan caps its work at FileRecord's internal max_orphan_scan_blocks
// instead of the attribute's own (attacker-controlled) declared block count,
// and logs once when the cap binds. The 3 real blocks stay reachable either
// way.
template <Cache::Strategy S>
void RunOrphanScanCapsDeclaredBlockCount() {
  auto reader = std::make_unique<MemoryDiskReader>(
      BuildFakeNtfsImageWithHugeOrphanScanBlockCount());
  const NtfsVolume<S> volume(
      std::move(reader),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  (void)TakeCapturedLog();
  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 2);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[0] == orphaned_block_reachable_name);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[1] == orphaned_block_orphan_name);
  CHECK_THAT(TakeCapturedLog(), ContainsSubstring("orphan scan capped at"));
}

// VG1: ScanOrphanedIndexBlocks() converts a block index to a VCN via
// clustersPerBlock = indexBlockSize / clusterSize. Every other orphan-scan
// fixture uses clustersPerBlock == 1, so this multiplication is otherwise
// never exercised - block 1 only resolves if its VCN is computed as 2
// (1 * clustersPerBlock), not 1.
template <Cache::Strategy S>
void RunMultiClusterOrphanScanConvertsBlockIndexToVcn() {
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(
          BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock()),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 2);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[0] == multi_cluster_reachable_name);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[1] == multi_cluster_orphan_name);
}

// Matrix row "Bad data run": a non-resident attribute's data run decodes
// one real run, then hits a decode error on the next. Strict: the
// attribute's constructor throws, so ParseAttrs() rejects the whole record
// and it exposes no $DATA attribute at all. Recovering: today's partial run
// list is kept - the attribute still parses, holding only the run decoded
// before the error.
template <Cache::Strategy S>
void RunBadDataRunRejectsOrKeepsPartial() {
  {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithBadDataRun()));
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::Data).empty());
  }
  {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithBadDataRun()),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    CHECK(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::Data).size() == 1);
  }
}

// VG2: AttachEfsContext()'s other anomaly branch (see
// RunBadDataRunRejectsOrKeepsPartial above for the sibling matrix row). Real
// NTFS never encrypts a resident $DATA (EFS only ever leaves file data
// non-resident), but a forged record could. Strict: the whole record is
// rejected. Recovering: the attribute is kept, read as is - it never gets
// an EFS context attached, so it is never decrypted.
template <Cache::Strategy S>
void RunResidentEncryptedDataRejectsOrKeepsAsIs() {
  {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        BuildFakeNtfsImageWithResidentEncryptedData()));
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::Data).empty());
  }
  {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(
            BuildFakeNtfsImageWithResidentEncryptedData()),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    CHECK(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::Data).size() == 1);
  }
}

// Matrix row "No end marker": the attribute walk runs out of record before
// ever finding a terminating Attr::Type::All marker. Strict: the whole record
// is rejected. Recovering: the attribute(s) already parsed before the walk
// ran out of room stay exposed.
template <Cache::Strategy S>
void RunNoEndMarkerRejectsOrKeepsParsed() {
  {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        BuildFakeNtfsImageWithNoEndMarker()));
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));

    (void)TakeCapturedLog();
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::Data).empty());
    CHECK_THAT(TakeCapturedLog(),
               ContainsSubstring(
                   "Attribute walk ended without a terminating end marker."));
  }
  {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithNoEndMarker()),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));

    (void)TakeCapturedLog();
    CHECK(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::Data).size() == 1);
    CHECK_THAT(TakeCapturedLog(),
               ContainsSubstring(
                   "Attribute walk ended without a terminating end marker."));
  }
}

// Matrix row "Bad entry in index block": an entry exceeds its
// $INDEX_ALLOCATION block's bounds mid-block. Strict: that one block is
// rejected whole, but the normal B+ tree walk still surfaces the
// unaffected sibling block ("Good"). Recovering: entries parsed before the
// bad one in the damaged block are reported too ("First", then "Good").
template <Cache::Strategy S>
void RunBadIndexBlockEntrySkipsBlockOrKeepsPrefix() {
  const auto traverse = [](NtfsBrowser::Io::FileRecord<S>& root) {
    std::vector<std::wstring> names;
    root.TraverseSubEntries(
        [](const IndexEntryView& index_entry, void* context) {
          static_cast<std::vector<std::wstring>*>(context)->emplace_back(
              index_entry.GetFilename());
        },
        &names);
    return names;
  };

  {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        BuildFakeNtfsImageWithBadIndexBlockEntry()));
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> root(volume);
    root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    REQUIRE(root.ParseAttrs());

    const std::vector<std::wstring> names = traverse(root);
    REQUIRE(names.size() == 1);
    // The REQUIRE above checks the size of names.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(names[0] == bad_index_block_good_name);
  }
  {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
                                   BuildFakeNtfsImageWithBadIndexBlockEntry()),
                               VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> root(volume);
    root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    REQUIRE(root.ParseAttrs());

    const std::vector<std::wstring> names = traverse(root);
    REQUIRE(names.size() == 2);
    // The REQUIRE above checks the size of names.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(names[0] == bad_index_block_first_name);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(names[1] == bad_index_block_good_name);
  }
}

// The entries of an $INDEX_ROOT attribute, cast down from its type-erased
// AttrBase<S> to the concrete AttrIndexRoot<RESIDENT, S> - itself a
// std::vector<IndexEntry> - the way FileRecord<S>'s own code does.
template <Cache::Strategy S>
const std::vector<IndexEntryView>&
    RootEntries(const NtfsBrowser::Attr::AttrBase<S>& attr) {
  if constexpr (S == Cache::Strategy::NoCache) {
    return static_cast<
        const AttrIndexRoot<AttrResidentNoCache, Cache::Strategy::NoCache>&>(
        attr);
  } else {
    return static_cast<const AttrIndexRoot<AttrResidentFullCache,
                                           Cache::Strategy::FullCache>&>(attr);
  }
}

// Matrix row "Malformed index entry": a $FILE_NAME stream inside an index
// entry overflows the entry's own declared size. Strict: the enclosing
// $INDEX_ROOT is rejected outright (AttrIndexRoot's constructor throws).
// Recovering: the entry is still kept, but nameless (GetFilename() empty),
// exactly as today.
template <Cache::Strategy S>
void RunMalformedIndexEntryRejectsOrKeepsNameless() {
  {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        BuildFakeNtfsImageWithMalformedIndexEntryFilename()));
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> root(volume);
    root.SetAttrMask(Attr::Mask::IndexRoot);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    CHECK_FALSE(root.ParseAttrs());
    CHECK(root.GetAttr(Attr::Type::IndexRoot).empty());
  }
  {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(
            BuildFakeNtfsImageWithMalformedIndexEntryFilename()),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    NtfsBrowser::Io::FileRecord<S> root(volume);
    root.SetAttrMask(Attr::Mask::IndexRoot);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    REQUIRE(root.ParseAttrs());

    const auto& root_attrs = root.GetAttr(Attr::Type::IndexRoot);
    REQUIRE(root_attrs.size() == 1);
    const std::vector<IndexEntryView>& entries =
        RootEntries<S>(*root_attrs.front());
    REQUIRE(entries.size() == 1);
    // The REQUIRE above checks the size of entries.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(entries[0].GetFileReference() == malformed_index_entry_mft_ref);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(entries[0].GetFilename().empty());
  }
}

// VG6: bypass_deleted_gate_ lets NtfsVolume::Init() read its own internal
// $Volume FileRecord even when that record is freed, under default
// VolumeOptions (include_deleted off) - unlike an ordinary file record,
// which the deleted gate would empty out.
template <Cache::Strategy S>
void RunDeletedVolumeRecordStillOpens() {
  const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
      BuildFakeNtfsImageWithDeletedVolumeRecord()));
  CHECK(volume.IsVolumeOK());
  CHECK(volume.GetVersion() == std::pair<BYTE, BYTE>{3, 1});
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("VolumeOptions default to both flags off",
                       "[volume-options]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunDefaultsAreBothOff<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume::GetOptions reflects the constructor argument",
    "[volume-options]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  RunGetOptionsReflectsConstructorArgument<S>();
}

TEMPLATE_TEST_CASE_SIG("include_deleted gates a freed record's attributes",
                       "[volume-options][regression]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunDeletedRecordContentGating<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A salvageable condition logs Warn when strict and Info when recovering",
    "[volume-options][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunSalvageableConditionLogLevel<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "The orphan scan caps a forged $INDEX_ALLOCATION block count",
    "[volume-options][file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanScanCapsDeclaredBlockCount<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "The orphan scan converts a multi-cluster index block's index to a VCN",
    "[volume-options][file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunMultiClusterOrphanScanConvertsBlockIndexToVcn<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A bad data run rejects the record by default, keeps a partial run "
    "list when recovering",
    "[volume-options][attr-non-resident][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunBadDataRunRejectsOrKeepsPartial<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A resident $DATA flagged encrypted rejects the record by default, "
    "keeps it read as is when recovering",
    "[volume-options][file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunResidentEncryptedDataRejectsOrKeepsAsIs<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A missing end-of-attributes marker rejects the record by default, "
    "keeps what parsed when recovering",
    "[volume-options][file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunNoEndMarkerRejectsOrKeepsParsed<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A bad entry in an index block skips that block by default, keeps its "
    "prefix when recovering, and never affects the sibling block",
    "[volume-options][attr-index-alloc][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunBadIndexBlockEntrySkipsBlockOrKeepsPrefix<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A malformed index entry rejects $INDEX_ROOT by default, keeps it "
    "nameless when recovering",
    "[volume-options][index-entry][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunMalformedIndexEntryRejectsOrKeepsNameless<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A freed $Volume record still opens the volume under default "
    "VolumeOptions",
    "[volume-options][ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunDeletedVolumeRecordStillOpens<S>();
}
