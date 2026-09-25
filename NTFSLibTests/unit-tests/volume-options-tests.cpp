#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/log.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "attr-index-root.h"
#include "attr-resident.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
using NtfsBrowser::AttrBase;
using NtfsBrowser::AttrIndexRoot;
using NtfsBrowser::AttrResidentFullCache;
using NtfsBrowser::AttrResidentNoCache;
using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Enum::MftIdx;
using namespace NtfsBrowserTests;

namespace
{

// VolumeOptions{} default-constructed: both flags off.
template <Strategy S>
void RunDefaultsAreBothOff()
{
  NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());
  CHECK_FALSE(volume.GetOptions().include_deleted);
  CHECK_FALSE(volume.GetOptions().recover_errors);
}

// GetOptions() reflects exactly what the constructor was given.
template <Strategy S>
void RunGetOptionsReflectsConstructorArgument()
{
  NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImage()),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetOptions().include_deleted);
  CHECK(volume.GetOptions().recover_errors);
}

// Decision 1: include_deleted off also gates a freed record's content.
// ParseFileRecord() still reads its header (IsDeleted() works), but
// ParseAttrs() exposes nothing unless include_deleted is on.
template <Strategy S>
void RunDeletedRecordContentGating()
{
  {
    NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithMftTree()));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(kMftTreeDeletedFileIdx));
    CHECK(record.IsDeleted());
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.getAttr(AttrType::FILE_NAME).empty());
  }
  {
    NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithMftTree()),
        VolumeOptions{.include_deleted = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(kMftTreeDeletedFileIdx));
    CHECK(record.IsDeleted());
    CHECK(record.ParseAttrs());
    CHECK_FALSE(record.getAttr(AttrType::FILE_NAME).empty());
  }
}

// A salvageable condition (here, a masked-in attribute's name exceeding its
// own total_size) logs at Warn when strict and Info when recovering, with
// the same text either way.
template <Strategy S>
void RunSalvageableConditionLogLevel()
{
  const std::shared_ptr<spdlog::logger> logger =
      spdlog::get(std::string(NtfsBrowser::Log::kLoggerName));
  REQUIRE(logger);

  auto captureFor = [&](const VolumeOptions& options)
  {
    std::ostringstream out;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(out);
    sink->set_pattern("%l %v");
    logger->sinks().push_back(sink);

    NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
                             BuildFakeNtfsImageWithAttrNameExceedsTotalSize()),
                         options);
    REQUIRE(volume.IsVolumeOK());
    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(kAttrNameExceedsTotalSizeRecordIdx));
    (void)record.ParseAttrs();

    logger->sinks().pop_back();
    return out.str();
  };

  const std::string strict = captureFor({});
  CHECK_THAT(strict,
             ContainsSubstring("warning Attribute name exceeds attribute "
                               "bounds."));

  const std::string recovering =
      captureFor(VolumeOptions{.recover_errors = true});
  CHECK_THAT(recovering,
             ContainsSubstring("info Attribute name exceeds attribute "
                               "bounds."));
  CHECK_THAT(recovering,
             !ContainsSubstring("warning Attribute name exceeds attribute "
                                "bounds."));
}

// The orphan scan caps its work at FileRecord::kMaxOrphanScanBlocks instead
// of the attribute's own (attacker-controlled) declared block count, and
// logs once when the cap binds. The 3 real blocks stay reachable either way.
template <Strategy S>
void RunOrphanScanCapsDeclaredBlockCount()
{
  auto reader = std::make_unique<MemoryDiskReader>(
      BuildFakeNtfsImageWithHugeOrphanScanBlockCount());
  NtfsVolume<S> volume(
      std::move(reader),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  (void)TakeCapturedLog();
  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntry& ie, void* context)
      {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            ie.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 2);
  CHECK(names[0] == kOrphanedBlockReachableName);
  CHECK(names[1] == kOrphanedBlockOrphanName);
  CHECK_THAT(TakeCapturedLog(), ContainsSubstring("orphan scan capped at"));
}

// VG1: ScanOrphanedIndexBlocks() converts a block index to a VCN via
// clustersPerBlock = indexBlockSize / clusterSize. Every other orphan-scan
// fixture uses clustersPerBlock == 1, so this multiplication is otherwise
// never exercised - block 1 only resolves if its VCN is computed as 2
// (1 * clustersPerBlock), not 1.
template <Strategy S>
void RunMultiClusterOrphanScanConvertsBlockIndexToVcn()
{
  NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(
          BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock()),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntry& ie, void* context)
      {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            ie.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 2);
  CHECK(names[0] == kMultiClusterReachableName);
  CHECK(names[1] == kMultiClusterOrphanName);
}

// Matrix row "Bad data run": a non-resident attribute's data run decodes
// one real run, then hits a decode error on the next. Strict: the
// attribute's constructor throws, so ParseAttrs() rejects the whole record
// and it exposes no $DATA attribute at all. Recovering: today's partial run
// list is kept - the attribute still parses, holding only the run decoded
// before the error.
template <Strategy S>
void RunBadDataRunRejectsOrKeepsPartial()
{
  {
    NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithBadDataRun()));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.getAttr(AttrType::DATA).empty());
  }
  {
    NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithBadDataRun()),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    CHECK(record.ParseAttrs());
    CHECK(record.getAttr(AttrType::DATA).size() == 1);
  }
}

// VG2: AttachEfsContext()'s other anomaly branch (see
// RunBadDataRunRejectsOrKeepsPartial above for the sibling matrix row). Real
// NTFS never encrypts a resident $DATA (EFS only ever leaves file data
// non-resident), but a forged record could. Strict: the whole record is
// rejected. Recovering: the attribute is kept, read as is - it never gets
// an EFS context attached, so it is never decrypted.
template <Strategy S>
void RunResidentEncryptedDataRejectsOrKeepsAsIs()
{
  {
    NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        BuildFakeNtfsImageWithResidentEncryptedData()));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.getAttr(AttrType::DATA).empty());
  }
  {
    NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
                             BuildFakeNtfsImageWithResidentEncryptedData()),
                         VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    CHECK(record.ParseAttrs());
    CHECK(record.getAttr(AttrType::DATA).size() == 1);
  }
}

// Matrix row "No end marker": the attribute walk runs out of record before
// ever finding a terminating AttrType::ALL marker. Strict: the whole record
// is rejected. Recovering: the attribute(s) already parsed before the walk
// ran out of room stay exposed.
template <Strategy S>
void RunNoEndMarkerRejectsOrKeepsParsed()
{
  {
    NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        BuildFakeNtfsImageWithNoEndMarker()));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

    (void)TakeCapturedLog();
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.getAttr(AttrType::DATA).empty());
    CHECK_THAT(TakeCapturedLog(),
               ContainsSubstring(
                   "Attribute walk ended without a terminating end marker."));
  }
  {
    NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithNoEndMarker()),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

    (void)TakeCapturedLog();
    CHECK(record.ParseAttrs());
    CHECK(record.getAttr(AttrType::DATA).size() == 1);
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
template <Strategy S>
void RunBadIndexBlockEntrySkipsBlockOrKeepsPrefix()
{
  auto traverse = [](FileRecord<S>& root)
  {
    std::vector<std::wstring> names;
    root.TraverseSubEntries(
        [](const IndexEntry& ie, void* context)
        {
          static_cast<std::vector<std::wstring>*>(context)->emplace_back(
              ie.GetFilename());
        },
        &names);
    return names;
  };

  {
    NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        BuildFakeNtfsImageWithBadIndexBlockEntry()));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> root(volume);
    root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    REQUIRE(root.ParseAttrs());

    const std::vector<std::wstring> names = traverse(root);
    REQUIRE(names.size() == 1);
    CHECK(names[0] == kBadIndexBlockGoodName);
  }
  {
    NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
                             BuildFakeNtfsImageWithBadIndexBlockEntry()),
                         VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> root(volume);
    root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    REQUIRE(root.ParseAttrs());

    const std::vector<std::wstring> names = traverse(root);
    REQUIRE(names.size() == 2);
    CHECK(names[0] == kBadIndexBlockFirstName);
    CHECK(names[1] == kBadIndexBlockGoodName);
  }
}

// The entries of an $INDEX_ROOT attribute, cast down from its type-erased
// AttrBase<S> to the concrete AttrIndexRoot<RESIDENT, S> - itself a
// std::vector<IndexEntry> - the way FileRecord<S>'s own code does.
template <Strategy S>
const std::vector<IndexEntry>& RootEntries(const AttrBase<S>& attr)
{
  if constexpr (S == Strategy::NO_CACHE)
  {
    return static_cast<
        const AttrIndexRoot<AttrResidentNoCache, Strategy::NO_CACHE>&>(attr);
  }
  else
  {
    return static_cast<
        const AttrIndexRoot<AttrResidentFullCache, Strategy::FULL_CACHE>&>(
        attr);
  }
}

// Matrix row "Malformed index entry": a $FILE_NAME stream inside an index
// entry overflows the entry's own declared size. Strict: the enclosing
// $INDEX_ROOT is rejected outright (AttrIndexRoot's constructor throws).
// Recovering: the entry is still kept, but nameless (GetFilename() empty),
// exactly as today.
template <Strategy S>
void RunMalformedIndexEntryRejectsOrKeepsNameless()
{
  {
    NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        BuildFakeNtfsImageWithMalformedIndexEntryFilename()));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> root(volume);
    root.SetAttrMask(Mask::INDEX_ROOT);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    CHECK_FALSE(root.ParseAttrs());
    CHECK(root.getAttr(AttrType::INDEX_ROOT).empty());
  }
  {
    NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(
            BuildFakeNtfsImageWithMalformedIndexEntryFilename()),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> root(volume);
    root.SetAttrMask(Mask::INDEX_ROOT);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    REQUIRE(root.ParseAttrs());

    const auto& rootAttrs = root.getAttr(AttrType::INDEX_ROOT);
    REQUIRE(rootAttrs.size() == 1);
    const std::vector<IndexEntry>& entries = RootEntries<S>(*rootAttrs.front());
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].GetFileReference() == kMalformedIndexEntryMftRef);
    CHECK(entries[0].GetFilename().empty());
  }
}

// VG6: bypass_deleted_gate_ lets NtfsVolume::Init() read its own internal
// $Volume FileRecord even when that record is freed, under default
// VolumeOptions (include_deleted off) - unlike an ordinary file record,
// which the deleted gate would empty out.
template <Strategy S>
void RunDeletedVolumeRecordStillOpens()
{
  NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
      BuildFakeNtfsImageWithDeletedVolumeRecord()));
  CHECK(volume.IsVolumeOK());
  CHECK(volume.GetVersion() == std::pair<BYTE, BYTE>{3, 1});
}

}  // namespace

TEST_CASE("VolumeOptions default to both flags off", "[volume-options]")
{
  RunDefaultsAreBothOff<Strategy::NO_CACHE>();
}

TEST_CASE("VolumeOptions default to both flags off (FULL_CACHE)",
          "[volume-options]")
{
  RunDefaultsAreBothOff<Strategy::FULL_CACHE>();
}

TEST_CASE("NtfsVolume::GetOptions reflects the constructor argument",
          "[volume-options]")
{
  RunGetOptionsReflectsConstructorArgument<Strategy::NO_CACHE>();
}

TEST_CASE(
    "NtfsVolume::GetOptions reflects the constructor argument (FULL_CACHE)",
    "[volume-options]")
{
  RunGetOptionsReflectsConstructorArgument<Strategy::FULL_CACHE>();
}

TEST_CASE("include_deleted gates a freed record's attributes",
          "[volume-options][regression]")
{
  RunDeletedRecordContentGating<Strategy::NO_CACHE>();
}

TEST_CASE("include_deleted gates a freed record's attributes (FULL_CACHE)",
          "[volume-options][regression]")
{
  RunDeletedRecordContentGating<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "A salvageable condition logs Warn when strict and Info when recovering",
    "[volume-options][regression]")
{
  RunSalvageableConditionLogLevel<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A salvageable condition logs Warn when strict and Info when recovering "
    "(FULL_CACHE)",
    "[volume-options][regression]")
{
  RunSalvageableConditionLogLevel<Strategy::FULL_CACHE>();
}

TEST_CASE("The orphan scan caps a forged $INDEX_ALLOCATION block count",
          "[volume-options][file-record][regression]")
{
  RunOrphanScanCapsDeclaredBlockCount<Strategy::NO_CACHE>();
}

TEST_CASE(
    "The orphan scan caps a forged $INDEX_ALLOCATION block count "
    "(FULL_CACHE)",
    "[volume-options][file-record][regression]")
{
  RunOrphanScanCapsDeclaredBlockCount<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "The orphan scan converts a multi-cluster index block's index to a VCN",
    "[volume-options][file-record][regression]")
{
  RunMultiClusterOrphanScanConvertsBlockIndexToVcn<Strategy::NO_CACHE>();
}

TEST_CASE(
    "The orphan scan converts a multi-cluster index block's index to a VCN "
    "(FULL_CACHE)",
    "[volume-options][file-record][regression]")
{
  RunMultiClusterOrphanScanConvertsBlockIndexToVcn<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "A bad data run rejects the record by default, keeps a partial run "
    "list when recovering",
    "[volume-options][attr-non-resident][regression]")
{
  RunBadDataRunRejectsOrKeepsPartial<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A bad data run rejects the record by default, keeps a partial run "
    "list when recovering (FULL_CACHE)",
    "[volume-options][attr-non-resident][regression]")
{
  RunBadDataRunRejectsOrKeepsPartial<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "A resident $DATA flagged encrypted rejects the record by default, "
    "keeps it read as is when recovering",
    "[volume-options][file-record][regression]")
{
  RunResidentEncryptedDataRejectsOrKeepsAsIs<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A resident $DATA flagged encrypted rejects the record by default, "
    "keeps it read as is when recovering (FULL_CACHE)",
    "[volume-options][file-record][regression]")
{
  RunResidentEncryptedDataRejectsOrKeepsAsIs<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "A missing end-of-attributes marker rejects the record by default, "
    "keeps what parsed when recovering",
    "[volume-options][file-record][regression]")
{
  RunNoEndMarkerRejectsOrKeepsParsed<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A missing end-of-attributes marker rejects the record by default, "
    "keeps what parsed when recovering (FULL_CACHE)",
    "[volume-options][file-record][regression]")
{
  RunNoEndMarkerRejectsOrKeepsParsed<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "A bad entry in an index block skips that block by default, keeps its "
    "prefix when recovering, and never affects the sibling block",
    "[volume-options][attr-index-alloc][regression]")
{
  RunBadIndexBlockEntrySkipsBlockOrKeepsPrefix<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A bad entry in an index block skips that block by default, keeps its "
    "prefix when recovering, and never affects the sibling block "
    "(FULL_CACHE)",
    "[volume-options][attr-index-alloc][regression]")
{
  RunBadIndexBlockEntrySkipsBlockOrKeepsPrefix<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "A malformed index entry rejects $INDEX_ROOT by default, keeps it "
    "nameless when recovering",
    "[volume-options][index-entry][regression]")
{
  RunMalformedIndexEntryRejectsOrKeepsNameless<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A malformed index entry rejects $INDEX_ROOT by default, keeps it "
    "nameless when recovering (FULL_CACHE)",
    "[volume-options][index-entry][regression]")
{
  RunMalformedIndexEntryRejectsOrKeepsNameless<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "A freed $Volume record still opens the volume under default "
    "VolumeOptions",
    "[volume-options][ntfs-volume][regression]")
{
  RunDeletedVolumeRecordStillOpens<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A freed $Volume record still opens the volume under default "
    "VolumeOptions (FULL_CACHE)",
    "[volume-options][ntfs-volume][regression]")
{
  RunDeletedVolumeRecordStillOpens<Strategy::FULL_CACHE>();
}
