#include <ntfs-browser/win-types.h>

#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "level-recorder.h"
#include "memory-disk-reader.h"
#include "nonresident-index-support.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// The ways another file can leave a list damaged.
constexpr auto overwritten_forms = {Tests::FakeListBytes::InvalidTypeFirst,
                                    Tests::FakeListBytes::InvalidTypeSecond,
                                    Tests::FakeListBytes::ZeroRecordSize,
                                    Tests::FakeListBytes::RecordSizeBelowHeader,
                                    Tests::FakeListBytes::RecordSizePastEnd,
                                    Tests::FakeListBytes::CutLastEntry};

// $FILE_NAME attributes of the listed file that the walk of its list imports
// before it meets the damage bytes made.
size_t NamesBeforeDamage(Tests::FakeListBytes bytes) {
  switch (bytes) {
    case Tests::FakeListBytes::Intact:
    case Tests::FakeListBytes::CutLastEntry:
      return 2;
    case Tests::FakeListBytes::InvalidTypeFirst:
      return 0;
    case Tests::FakeListBytes::InvalidTypeSecond:
    case Tests::FakeListBytes::ZeroRecordSize:
    case Tests::FakeListBytes::RecordSizeBelowHeader:
    case Tests::FakeListBytes::RecordSizePastEnd:
      return 1;
  }
  return 0;
}

// The listed file of the fixture, with its list in the state and residency
// asked for, whose attributes are not parsed yet.
template <Cache::Strategy S>
ParsedRecord<S> OpenListed(Tests::FakeListResidency residency,
                           Tests::FakeListedState state,
                           Tests::FakeListBytes bytes, bool recover) {
  return OpenRecord<S>(
      Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
          Tests::FakeListedExtension::Genuine, residency, state, bytes),
      Tests::mft_tree_list_base_idx,
      VolumeOptions{.include_deleted = true, .recover_errors = recover});
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A freed record whose non-resident $ATTRIBUTE_LIST another file "
    "overwrote keeps its other attributes, quietly",
    "[attr-list][overwritten]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const bool recover = GENERATE(false, true);
  const Tests::FakeListBytes bytes = GENERATE(from_range(overwritten_forms));
  const ParsedRecord<S> opened =
      OpenListed<S>(Tests::FakeListResidency::NonResident,
                    Tests::FakeListedState::Freed, bytes, recover);

  const Tests::RecordLevels levels;
  CHECK(opened.record->ParseAttrs());

  CHECK(opened.record->HasOverwrittenAttrList());
  CHECK(opened.record->GetAttr(Attr::Type::StandardInformation).size() == 1);
  CHECK(opened.record->GetAttr(Attr::Type::FileName).size() ==
        NamesBeforeDamage(bytes));
  CHECK_FALSE(opened.record->IsSalvaged());
  CHECK_FALSE(levels.Levels().Logged(Log::Level::Warn));
  CHECK_FALSE(levels.Levels().Logged(Log::Level::Error));
  CHECK(levels.Levels().Count(Log::Level::Debug, "was overwritten") == 1);
}

TEMPLATE_TEST_CASE_SIG(
    "A freed record's intact non-resident $ATTRIBUTE_LIST is not overwritten",
    "[attr-list][overwritten]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const bool recover = GENERATE(false, true);
  const ParsedRecord<S> opened = OpenListed<S>(
      Tests::FakeListResidency::NonResident, Tests::FakeListedState::Freed,
      Tests::FakeListBytes::Intact, recover);

  CHECK(opened.record->ParseAttrs());

  CHECK_FALSE(opened.record->HasOverwrittenAttrList());
  CHECK(opened.record->GetAttr(Attr::Type::FileName).size() == 2);
}

TEMPLATE_TEST_CASE_SIG(
    "A damaged $ATTRIBUTE_LIST of a live record, or a resident one, stays a "
    "defect",
    "[attr-list][overwritten]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const bool recover = GENERATE(false, true);
  const auto [residency, state] =
      GENERATE(std::pair{Tests::FakeListResidency::NonResident,
                         Tests::FakeListedState::Live},
               std::pair{Tests::FakeListResidency::Resident,
                         Tests::FakeListedState::Freed},
               std::pair{Tests::FakeListResidency::Resident,
                         Tests::FakeListedState::Live});

  SECTION("an attribute type that is not one") {
    const ParsedRecord<S> opened = OpenListed<S>(
        residency, state, Tests::FakeListBytes::InvalidTypeFirst, recover);
    const Tests::RecordLevels levels;

    CHECK_FALSE(opened.record->ParseAttrs());

    CHECK_FALSE(opened.record->HasOverwrittenAttrList());
    CHECK(levels.Levels().Count(Log::Level::Error, "al_record.attr_type") == 1);
    CHECK(levels.Levels().Count(Log::Level::Debug, "was overwritten") == 0);
  }

  SECTION("a cut last entry") {
    const ParsedRecord<S> opened = OpenListed<S>(
        residency, state, Tests::FakeListBytes::CutLastEntry, recover);
    const Tests::RecordLevels levels;

    // Strict rejects the cut list. Recovering keeps what it read.
    CHECK(opened.record->ParseAttrs() == recover);

    CHECK_FALSE(opened.record->HasOverwrittenAttrList());
    CHECK(
        levels.Levels().Logged(recover ? Log::Level::Info : Log::Level::Warn));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "An overwritten $ATTRIBUTE_LIST of a freed record costs the reads that the "
    "same list of a live record costs",
    "[attr-list][overwritten]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const bool recover = GENERATE(false, true);
  const Tests::FakeListBytes bytes = GENERATE(from_range(overwritten_forms));
  const auto reads_of = [&](Tests::FakeListedState state) {
    auto reader = std::make_unique<Tests::CountingDiskReader>(
        std::make_unique<Tests::MemoryDiskReader>(
            Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
                Tests::FakeListedExtension::Genuine,
                Tests::FakeListResidency::NonResident, state, bytes)));
    const Tests::CountingDiskReader* const counting = reader.get();
    const ParsedRecord<S> opened = OpenRecordFrom<S>(
        std::move(reader), Tests::mft_tree_list_base_idx,
        VolumeOptions{.include_deleted = true, .recover_errors = recover});
    const size_t before = counting->Calls().size();
    (void)opened.record->ParseAttrs();
    return counting->Calls().size() - before;
  };

  CHECK(reads_of(Tests::FakeListedState::Freed) ==
        reads_of(Tests::FakeListedState::Live));
}
