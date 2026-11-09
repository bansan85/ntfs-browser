#include "usn-journal-scenarios.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using FakeImage::FakeUsnOptions;
using FakeImage::FakeUsnPatch;
using FakeImage::FakeUsnRecord;

// One saved input: its file name, and the journal the image holds.
struct Variant {
  std::string name;
  FakeUsnOptions options;
};

// Where a field sits in a V2 record.
constexpr std::size_t length_offset = 0;
constexpr std::size_t major_version_offset = 4;
constexpr std::size_t name_length_offset = 56;

// A major version no journal has.
constexpr WORD unknown_version = 9;

// Records of the usual journal: six, so that a damaged third has records on
// both sides.
constexpr std::size_t usual_record_count = 6;

// Fourteen records fill the first cluster of the journal.
constexpr std::size_t cluster_filling_count = 14;

// A journal of count records with names of the same length, so each takes the
// same number of bytes.
std::vector<FakeUsnRecord> Records(std::size_t count) {
  std::vector<FakeUsnRecord> records;
  for (std::size_t i = 0; i < count; ++i) {
    records.push_back(
        {.name = L"file" + std::to_wstring(100 + i), .file_record = 100 + i});
  }
  return records;
}

// Offset of record index in a stream of Records(): they all take the same size.
std::size_t StartOf(std::size_t index) {
  const std::size_t size =
      FakeImage::EncodeUsnRecord(Records(1).front(), 0).size();
  return index * size;
}

// A patch that writes value over the field at field_offset of a record.
template <class T>
FakeUsnPatch Patch(std::size_t record_index, std::size_t field_offset,
                   T value) {
  std::vector<BYTE> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return {.stream_offset = StartOf(record_index) + field_offset,
          .bytes = std::move(bytes)};
}

// The inputs, one per way a hostile $J can be wrong.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;

  FakeUsnOptions forged{.records = Records(usual_record_count)};
  forged.patches.push_back(Patch<DWORD>(2, length_offset, 4000));
  variants.push_back({"usn_journal_forged_length", std::move(forged)});

  FakeUsnOptions zero{.records = Records(usual_record_count)};
  zero.patches.push_back(Patch<DWORD>(2, length_offset, 0));
  variants.push_back({"usn_journal_zero_length", std::move(zero)});

  FakeUsnOptions past{.records = Records(usual_record_count)};
  past.patches.push_back(Patch<WORD>(2, name_length_offset, 2000));
  variants.push_back({"usn_journal_name_past_record", std::move(past)});

  std::vector<FakeUsnRecord> with_v4 = Records(usual_record_count);
  with_v4.at(2).major_version = 4;
  variants.push_back(
      {"usn_journal_v4_record", {.records = std::move(with_v4)}});

  std::vector<FakeUsnRecord> mixed = Records(usual_record_count - 1);
  mixed.at(1).major_version = 3;
  mixed.at(2).major_version = 4;
  mixed.at(4).major_version = 3;
  variants.push_back(
      {"usn_journal_mixed_versions", {.records = std::move(mixed)}});

  // The last record declares more bytes than are left of the cluster.
  FakeUsnOptions tail{.records = Records(cluster_filling_count)};
  tail.patches.push_back(
      Patch<DWORD>(cluster_filling_count - 1, length_offset, 1000));
  variants.push_back({"usn_journal_truncated_tail", std::move(tail)});

  // The first damaged record starts a stretch that only a record whose USN is
  // its own offset ends: the next one has another.
  std::vector<FakeUsnRecord> mismatch = Records(usual_record_count);
  mismatch.at(3).usn = 777;
  FakeUsnOptions anchor{.records = std::move(mismatch)};
  anchor.patches.push_back(
      Patch<WORD>(2, major_version_offset, unknown_version));
  variants.push_back({"usn_journal_anchor_mismatch", std::move(anchor)});

  variants.push_back({"usn_journal_sparse_head",
                      {.records = Records(usual_record_count),
                       .sparse_head_bytes = ULONGLONG{1} << 30U}});
  return variants;
}

}  // namespace

void UsnJournalScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown: only then does a scan read the
  // journal.
  const auto options = RecordedStream::Recovering();
  for (const Variant& variant : Variants()) {
    registry.AddRecorded(
        variant.name,
        [journal = variant.options] {
          std::vector<BYTE> image =
              FakeImage::BuildFakeNtfsImageWithFreedFileTree();
          FakeImage::PutUsnJournal(image, journal);
          return image;
        },
        options);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
