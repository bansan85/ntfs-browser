#include <ntfs-browser/win-types.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "ntfs-browser/log/sink.h"
#include "test-log-sink.h"
#include "usn-test-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Usn = NTFS_BROWSER_NAMESPACE::Usn;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeUsnRecord;

namespace {

using Clock = std::chrono::steady_clock;

// Seconds between two instants.
double SecondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// Takes the capturing sink out for as long as it lives: the library would
// otherwise keep every Trace line of a million-record read in memory, and the
// timing would measure that.
class QuietLog {
 public:
  QuietLog() { Log::SetSinks({}); }

  QuietLog(const QuietLog&) = delete;
  QuietLog& operator=(const QuietLog&) = delete;

  ~QuietLog() { Tests::InstallCaptureSink(); }
};

// A journal of count records with names of 20 characters: what a document
// folder of a user's disk looks like. Each takes 104 bytes.
std::vector<BYTE> RealisticStream(size_t count) {
  std::vector<BYTE> stream;
  stream.reserve(count * 104);
  for (size_t i = 0; i < count; ++i) {
    std::wstring name = L"document-" + std::to_wstring(1000000 + i) + L".do";
    const std::vector<BYTE> record = Tests::EncodeUsnRecord(
        FakeUsnRecord{.name = std::move(name), .file_record = 100 + i},
        stream.size());
    stream.insert(stream.end(), record.begin(), record.end());
  }
  return stream;
}

// Bytes the records hold: the structs, and the names that do not fit the
// string's own buffer.
size_t HeldBytes(const Usn::Journal& journal) {
  size_t held = journal.records.size() * sizeof(Usn::Record);
  for (const Usn::Record& record : journal.records) {
    constexpr size_t short_string_capacity = 7;
    if (record.name.capacity() > short_string_capacity) {
      held += (record.name.capacity() + 1) * sizeof(wchar_t);
    }
  }
  return held;
}

// Reads a journal of count records and prints what it took. Returns the
// seconds per record.
double MeasureJournal(size_t count) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutUsnJournal(image, {.raw_stream = RealisticStream(count)});
  const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(
      std::move(image),
      VolumeOptions{.include_deleted = true,
                    .recover_errors = true,
                    .max_usn_records = Usn::unlimited_records});

  const auto start = Clock::now();
  const Usn::Journal* journal = opened.volume->GetUsnJournal();
  const double seconds = SecondsSince(start);

  REQUIRE(journal != nullptr);
  const double per_record = seconds / static_cast<double>(count);
  std::cout << std::format(
      "journal of {} records: {:.2f} s, {:.2f} us a record, {:.0f} MiB "
      "held ({:.0f} bytes a record)\n",
      count, seconds, per_record * 1e6,
      static_cast<double>(HeldBytes(*journal)) / (1024.0 * 1024.0),
      static_cast<double>(HeldBytes(*journal)) / static_cast<double>(count));
  return per_record;
}

// Scans the $MFT of an image of many freed files, and returns the seconds per
// record.
double MeasureMftScan(size_t files) {
  const Tests::FakeManyDeleted many =
      Tests::BuildFakeNtfsImageWithManyDeletedFiles(files, 1);
  const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(
      many.image, VolumeOptions{.include_deleted = true});

  const auto start = Clock::now();
  const Mft::MftTree tree(*opened.volume);
  const double seconds = SecondsSince(start);

  const double per_record = seconds / static_cast<double>(tree.Stats().slots);
  std::cout << std::format(
      "$MFT scan of {} slots: {:.2f} s, {:.2f} us a record\n",
      tree.Stats().slots, seconds, per_record * 1e6);
  return per_record;
}

}  // namespace

TEST_CASE("the cost of reading a change journal", "[.benchmark]") {
  const QuietLog quiet;

  std::cout << std::format("sizeof(Usn::Record) = {} bytes\n",
                           sizeof(Usn::Record));
  const double journal_per_record = MeasureJournal(1000000);
  MeasureJournal(Usn::default_max_records);
  const double scan_per_record = MeasureMftScan(20000);

  // What a recovery scan adds on a volume of mft_records records with a journal
  // of journal_records: the journal's time over the scan's own.
  constexpr double mft_records = 6000000;
  constexpr double journal_records = 1000000;
  const double added =
      (journal_records * journal_per_record) / (mft_records * scan_per_record);
  std::cout << std::format(
      "a volume of {:.0f} MFT records and a journal of {:.0f} records: the "
      "journal adds {:.0f}% to the scan\n",
      mft_records, journal_records, added * 100.0);
}
