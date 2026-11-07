#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/attr/nonresident-index-support.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/level-recorder.h>
#include <ntfs-browser/volume-options.h>

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// The ways another file can leave a list damaged.
constexpr auto overwritten_forms = {
    TestFakeImage::FakeListBytes::InvalidTypeFirst,
    TestFakeImage::FakeListBytes::InvalidTypeSecond,
    TestFakeImage::FakeListBytes::ZeroRecordSize,
    TestFakeImage::FakeListBytes::RecordSizeBelowHeader,
    TestFakeImage::FakeListBytes::RecordSizePastEnd,
    TestFakeImage::FakeListBytes::CutLastEntry};

// The listed file of the fixture, with its list in the state and residency
// asked for, whose attributes are not parsed yet.
template <Cache::Strategy S>
ParsedRecord<S> OpenListed(TestFakeImage::FakeListResidency residency,
                           TestFakeImage::FakeListedState state,
                           TestFakeImage::FakeListBytes bytes, bool recover) {
  return OpenRecord<S>(
      TestFakeImage::BuildFakeNtfsImageWithMftTreeAttributeList(
          TestFakeImage::FakeListedExtension::Genuine, residency, state, bytes),
      TestFakeImage::mft_tree_list_base_idx,
      VolumeOptions{.include_deleted = true, .recover_errors = recover});
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A freed record whose non-resident $ATTRIBUTE_LIST another file "
    "overwrote keeps its other attributes, quietly",
    "[attr-list][overwritten]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const bool recover = GENERATE(false, true);
  const TestFakeImage::FakeListBytes bytes =
      GENERATE(from_range(overwritten_forms));
  const ParsedRecord<S> opened =
      OpenListed<S>(TestFakeImage::FakeListResidency::NonResident,
                    TestFakeImage::FakeListedState::Freed, bytes, recover);

  const Tests::RecordLevels levels;
  CHECK(opened.record->ParseAttrs());

  CHECK(opened.record->HasOverwrittenAttrList());
  CHECK(opened.record->GetAttr(Attr::Type::StandardInformation).size() == 1);
  CHECK(opened.record->GetAttr(Attr::Type::FileName).size() ==
        TestFakeImage::FakeListNamesBeforeDamage(bytes));
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
  const ParsedRecord<S> opened =
      OpenListed<S>(TestFakeImage::FakeListResidency::NonResident,
                    TestFakeImage::FakeListedState::Freed,
                    TestFakeImage::FakeListBytes::Intact, recover);

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
      GENERATE(std::pair{TestFakeImage::FakeListResidency::NonResident,
                         TestFakeImage::FakeListedState::Live},
               std::pair{TestFakeImage::FakeListResidency::Resident,
                         TestFakeImage::FakeListedState::Freed},
               std::pair{TestFakeImage::FakeListResidency::Resident,
                         TestFakeImage::FakeListedState::Live});

  SECTION("an attribute type that is not one") {
    const ParsedRecord<S> opened =
        OpenListed<S>(residency, state,
                      TestFakeImage::FakeListBytes::InvalidTypeFirst, recover);
    const Tests::RecordLevels levels;

    CHECK_FALSE(opened.record->ParseAttrs());

    CHECK_FALSE(opened.record->HasOverwrittenAttrList());
    CHECK(levels.Levels().Count(Log::Level::Error, "al_record.attr_type") == 1);
    CHECK(levels.Levels().Count(Log::Level::Debug, "was overwritten") == 0);
  }

  SECTION("a cut last entry") {
    const ParsedRecord<S> opened = OpenListed<S>(
        residency, state, TestFakeImage::FakeListBytes::CutLastEntry, recover);
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
  const TestFakeImage::FakeListBytes bytes =
      GENERATE(from_range(overwritten_forms));
  const auto reads_of = [&](TestFakeImage::FakeListedState state) {
    auto reader = std::make_unique<TestDiskReaders::CountingDiskReader>(
        std::make_unique<TestDiskReaders::MemoryDiskReader>(
            TestFakeImage::BuildFakeNtfsImageWithMftTreeAttributeList(
                TestFakeImage::FakeListedExtension::Genuine,
                TestFakeImage::FakeListResidency::NonResident, state, bytes)));
    const TestDiskReaders::CountingDiskReader* const counting = reader.get();
    const ParsedRecord<S> opened = OpenRecordFrom<S>(
        std::move(reader), TestFakeImage::mft_tree_list_base_idx,
        VolumeOptions{.include_deleted = true, .recover_errors = recover});
    const size_t before = counting->Calls().size();
    (void)opened.record->ParseAttrs();
    return counting->Calls().size() - before;
  };

  CHECK(reads_of(TestFakeImage::FakeListedState::Freed) ==
        reads_of(TestFakeImage::FakeListedState::Live));
}
