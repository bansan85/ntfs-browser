#include <memory>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

// Regression test for a bug in FileRecord::SetAttrMask(): it let a caller's
// mask silently exclude $ATTRIBUTE_LIST, even though the comment right
// above the assignment says "Standard Information and Attribute List is
// needed always". Real directories that outgrow their base MFT record (eg.
// C:\Windows) relocate $INDEX_ROOT/$INDEX_ALLOCATION into an extension
// record and leave only an $ATTRIBUTE_LIST pointer behind. NtfsDir.exe asks
// for Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION only, so that pointer was
// never followed and such directories silently listed as empty - eg.
// ".\NtfsDir.exe C:\windows" printed "Files: 0, Directories: 0" instead of
// C:\Windows' real contents.
//
// BuildFakeNtfsImageWithAttributeListDirectory() reproduces the same shape:
// kAttributeListDirIdx's only attribute is a resident $ATTRIBUTE_LIST
// pointing $INDEX_ROOT at kIndexExtensionIdx, which holds the actual
// "Foo" entry.
TEST_CASE("FindSubEntry follows $ATTRIBUTE_LIST to a relocated $INDEX_ROOT",
          "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDirectory());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  // Mirrors NtfsDir.exe's actual call: it asks for the two index
  // attributes only, not $ATTRIBUTE_LIST.
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  REQUIRE(dir.ParseAttrs());

  // Before the fix, $ATTRIBUTE_LIST was never parsed, so $INDEX_ROOT -
  // which only exists in the extension record - never got merged in here.
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  const std::optional<IndexEntry> found = dir.FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == 20);
}

// Regression test for F17's primary defect (docs/bug-reports/2026-09-03-full-repo.md):
// AttrList<S>::AttrList() (src/attr-list.cpp) deduplicates the set of
// resolved extension records (fr.attr_list_chain_) on record_ref alone, not
// on (record_ref, attr_type). A single extension record commonly hosts
// several relocated attributes of different types (eg. $INDEX_ROOT and
// $INDEX_ALLOCATION, as real directories like C:\Windows do once they
// outgrow their base record) - once the first $ATTRIBUTE_LIST entry
// resolves that record, every later entry pointing at the very same record
// is skipped outright, regardless of which attribute type it names.
//
// BuildFakeNtfsImageWithMultiTypeAttributeListDirectory() reproduces this:
// kAttrListMultiTypeDirIdx's $ATTRIBUTE_LIST has two entries, both naming
// kMultiTypeExtensionIdx - first for $INDEX_ROOT, then for
// $INDEX_ALLOCATION - and that single extension record holds both
// attributes.
TEST_CASE(
    "AttrList merges every attribute type relocated into the same "
    "extension record (F17)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMultiTypeAttributeListDirectory());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttrListMultiTypeDirIdx));
  REQUIRE(dir.ParseAttrs());

  // The first $ATTRIBUTE_LIST entry (relocating $INDEX_ROOT) is always
  // resolved fine, first-in-chain.
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  // Before the fix: the second entry, relocating $INDEX_ALLOCATION to the
  // SAME extension record, gets skipped because that record_ref was
  // already marked resolved by the first entry - even though
  // $INDEX_ALLOCATION itself was never actually retrieved.
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ALLOCATION).empty());
}

// Regression test for F17's secondary defect
// (docs/bug-reports/2026-09-03-full-repo.md): FileRecord::attr_list_chain_
// is lazily created but never reset by FileRecord::ParseFileRecord() (only
// ClearAttrs() + file_record_.reset() run there). Sample apps like
// ntfsdir.exe and ntfsundel.exe reuse a single FileRecord across many
// ParseFileRecord()/ParseAttrs() calls while walking a volume, so the set
// of "already resolved" extension records from one file's $ATTRIBUTE_LIST
// chain silently leaks into the next, unrelated file's chain.
//
// BuildFakeNtfsImageWithAttributeListDirectoryChainReused() reproduces
// this: a second, independent directory (kAttributeListDirIdx2) whose own
// $ATTRIBUTE_LIST also relocates $INDEX_ROOT to kIndexExtensionIdx - the
// very same extension record the first directory
// (kAttributeListDirIdx) already resolved via the same, reused
// FileRecord object.
TEST_CASE(
    "AttrList chain state does not leak across FileRecord::ParseFileRecord "
    "calls on a reused FileRecord (F17)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListDirectoryChainReused());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  // First parse: a fresh FileRecord resolves $INDEX_ROOT through
  // kIndexExtensionIdx normally.
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  // Second parse, same FileRecord object, unrelated directory whose own
  // $ATTRIBUTE_LIST also relocates $INDEX_ROOT to kIndexExtensionIdx.
  // Before the fix: attr_list_chain_ survives from the first parse still
  // containing kIndexExtensionIdx, so this second, independent resolution
  // is wrongly skipped as "already resolved".
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx2));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}

// Regression test for N3 (docs/bug-reports/2026-09-03-full-repo.md, lines
// 87-96): AttrList<S>::AttrList() (src/attr-list.cpp) grows
// file_record_list_ - a std::vector<FileRecord<S>> - by one emplace_back()
// per resolved extension record. In Strategy::FULL_CACHE,
// FileRecordHeaderImpl<FULL_CACHE>::data_ (the raw 1024-byte record buffer)
// is a BY-VALUE member of FileRecord itself, and AttrBase::attr_header_ /
// AttrNonResident::attr_header_nr_ are references bound directly into that
// buffer at construction time. Once a later extension record's
// emplace_back() reallocates the vector, every FileRecord already
// constructed for an earlier entry - and every attribute already merged out
// of it into fr.attr_list_ - is relocated in memory, leaving those
// references dangling: a use-after-free. NO_CACHE is unaffected (its
// backing buffer, record_buffer_, is heap-allocated and stable across a
// move).
//
// BuildFakeNtfsImageWithFragmentedAttributeListDirectory() reproduces the
// nominal, non-forged scenario the report describes - an $ATTRIBUTE_LIST
// relocating attributes into several distinct extension records, as real
// directories like C:\Windows commonly do: kUafAttrListDirIdx's
// $ATTRIBUTE_LIST has FOUR entries, each naming a DIFFERENT extension
// record (kUafExtensionIdx0..3), each holding a minimal non-resident
// $INDEX_ALLOCATION whose real_size is a distinct, recognizable sentinel
// (kUafRealSizeSentinels). Four distinct records guarantee at least one
// std::vector reallocation regardless of the growth factor in use (eg.
// MSVC's ~1.5x would already reallocate by the 2nd-4th emplace_back).
TEST_CASE(
    "AttrList's FileRecord vector growth does not invalidate "
    "already-resolved extension records' attributes (N3, FULL_CACHE)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithFragmentedAttributeListDirectory());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kUafAttrListDirIdx));
  REQUIRE(dir.ParseAttrs());

  const auto& allocAttrs = dir.getAttr(AttrType::INDEX_ALLOCATION);
  REQUIRE(allocAttrs.size() == NtfsBrowserTests::kUafRealSizeSentinels.size());

  // Each resolved $INDEX_ALLOCATION's GetDataSize() reads
  // attr_header_nr_.real_size fresh, through the reference bound at that
  // extension record's construction time - exactly the dangling read N3
  // describes once a later record's resolution has moved it. The FIRST
  // extension record (kUafExtensionIdx0) is the one most certain to have
  // been relocated by later emplace_back() calls, so its sentinel is the
  // most telling, but every one is checked: before the fix, one or more no
  // longer match their expected sentinel.
  for (size_t i = 0; i < allocAttrs.size(); i++)
  {
    CHECK(allocAttrs[i]->GetDataSize() ==
          NtfsBrowserTests::kUafRealSizeSentinels[i]);
  }
}

// Regression test for the AttrList<S>::AttrList() hardening added alongside
// bug F10 (docs/bug-reports/2026-09-03-full-repo.md, src/attr-list.cpp):
// al_record is a single stack variable reused across the ctor's loop
// iterations, never reset between them. Before F10 was fixed,
// AttrResident<S>::ReadData() always reported the full requested length
// regardless of truncation, so the loop's own
// "*len == sizeof(Attr::AttributeList)" check could never actually detect a
// short final read - a genuine hazard while unfixed, since a truncated read
// leaves most of al_record holding STALE bytes from the previous iteration,
// yet the loop would still treat it as a fresh, valid entry. Fixing F10
// alone already makes that check correctly false for a short read (this
// fixture's whole point), and the added NTFS_TRACE2 call (verified via the
// fuzz corpus entry "attribute_list_short_read", see
// fuzzer-regression-tests.cpp) turns that already-correct stop into an
// observable one instead of a silent exit.
//
// BuildFakeNtfsImageWithAttributeListShortRead() reproduces the shape: the
// root directory's resident $ATTRIBUTE_LIST is 50 bytes - one full,
// self-referencing entry (40 bytes, so resolving it never needs an
// extension record) plus 10 leftover bytes, not an exact multiple of
// sizeof(Attr::AttributeList) (40). Either way (fixed or not), this
// particular fixture can never misbehave dangerously even if a short read
// were wrongly treated as a full entry: the stale bytes filling most of
// that phantom entry are themselves leftover from entry 1, whose own
// base_ref is this very record's self-reference, so a phantom entry built
// from them would also just be skipped harmlessly. What actually matters
// here is that ParseAttrs() completes cleanly either way.
TEST_CASE(
    "AttrList stops cleanly on a resident $ATTRIBUTE_LIST whose size isn't "
    "a multiple of the entry size (F10 hardening)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

// Same fixture under FULL_CACHE - AttrList<S>::AttrList()'s loop runs
// identically regardless of strategy (it only ever calls this->ReadData(),
// which both AttrResidentNoCache and AttrResidentFullCache implement via the
// same shared AttrResident<S>::ReadData()) - confirms the defect (and its
// fix) isn't an artifact of one strategy.
TEST_CASE(
    "AttrList stops cleanly on a resident $ATTRIBUTE_LIST whose size isn't "
    "a multiple of the entry size (F10 hardening, FULL_CACHE)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

// Regression test for AttrList<S>::AttrList()'s cycle guard
// (src/attr-list.cpp): two records whose resident $ATTRIBUTE_LIST each
// names the OTHER for attr_type ATTRIBUTE_LIST. Resolving the root
// directory's (#5) entry recurses into kAttrListCycleExtIdx's own
// ParseAttrs(), whose own $ATTRIBUTE_LIST entry names #5 again for the same
// attribute type - a key already inserted into attrListChain before #5's
// own AttrList ctor started resolving entries - so this must be recognized
// and skipped instead of recursing without bound (a real cycle, as opposed
// to the F17 tests above, which cover a shared but acyclic target).
// BuildFakeNtfsImageWithAttributeListCycle() also regenerates the fuzz
// corpus entry NTFSLibTests/fuzz/data/attribute_list_extension_record_cycle
// (see fuzzer-regression-tests.cpp) in a deterministic, from-scratch form.
TEST_CASE(
    "AttrList's cycle guard stops a two-record $ATTRIBUTE_LIST resolution "
    "cycle instead of recursing without bound",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListCycle());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

// Same fixture under FULL_CACHE.
TEST_CASE(
    "AttrList's cycle guard stops a two-record $ATTRIBUTE_LIST resolution "
    "cycle instead of recursing without bound (FULL_CACHE)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListCycle());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

// Regression test for bug F11's primary trigger scenario
// (docs/bug-reports/2026-09-03-full-repo.md): a resident $ATTRIBUTE_LIST
// whose real data size is an exact, tight multiple of the REAL 26-byte
// on-disk entry size but NOT a multiple of sizeof(Attr::AttributeList) (40
// pre-fix, still 32 post-fix due to alignof(ULONGLONG) padding with no
// on-disk counterpart). AttrList<S>::AttrList()'s read loop
// (src/attr-list.cpp) used to request sizeof(Attr::AttributeList) bytes per
// iteration; with only 26 real bytes per on-disk entry, its first (40-byte)
// read already consumed all of entry 1's real bytes plus 14 bytes belonging
// to entry 2, so the next iteration's request (offset 26, only 26 real
// bytes left) came up short - entry 2 was silently dropped even though
// nothing about the volume is malformed.
//
// BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory() reproduces
// this: kAttrListTightPackDirIdx's $ATTRIBUTE_LIST has two entries, packed
// back-to-back at the real 26-byte stride, relocating $INDEX_ROOT to
// kAttrListTightPackExtIdxA and $INDEX_ALLOCATION to a DIFFERENT record,
// kAttrListTightPackExtIdxB - entry 1 happens to still resolve pre-fix
// (every field the loop reads for it sits before the struct's buggy
// base_ref/attr_id region), which is what makes entry 2's silent drop easy
// to miss.
TEST_CASE(
    "AttrList resolves every entry in a densely-packed (real 26-byte "
    "stride) $ATTRIBUTE_LIST, not just those a multiple of "
    "sizeof(Attr::AttributeList) apart (F11)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttrListTightPackDirIdx));
  REQUIRE(dir.ParseAttrs());

  // Entry 1 ($INDEX_ROOT) resolves even pre-fix.
  const std::optional<IndexEntry> found = dir.FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == 20);

  // Entry 2 ($INDEX_ALLOCATION) is the one bug F11 silently drops: before
  // the fix, this vector is empty.
  const auto& allocAttrs = dir.getAttr(AttrType::INDEX_ALLOCATION);
  REQUIRE(allocAttrs.size() == 1);
  CHECK(allocAttrs[0]->GetDataSize() ==
        NtfsBrowserTests::kAttrListTightPackRealSize);
}
