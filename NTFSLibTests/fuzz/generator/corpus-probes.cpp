#include <ntfs-browser/win-types.h>

#include <array>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/corpus-probes.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

// The MFT records of the DFTT test #3 search-term files, and their sizes.
// Resident, non-resident, and non-resident across fragmented clusters.
constexpr ULONGLONG kw_resident_size = 120;
constexpr ULONGLONG kw_non_resident_size = 2000;
constexpr ULONGLONG kw_fragmented_size = 2600;

// The deleted search-term file, and the file with a term in its slack.
constexpr ULONGLONG kw_deleted_record = 34;
constexpr ULONGLONG kw_slack_record = 36;

// Where /1.txt of ntfs-ramslack.raw keeps its RAM slack and its cluster slack,
// from the ReadMe of the image, and the bytes the test reads there.
constexpr LONGLONG ram_slack_address = 213303;
constexpr LONGLONG cluster_slack_address = 213504;
constexpr ULONGLONG slack_probe_bytes = 4;

// The records of $MFT's own run list in ntfs_extremely_fragmented_mft.raw: its
// base record and its extension records.
constexpr std::initializer_list<ULONGLONG> fragmented_mft_records = {
    0,
    15,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    34'799'617ULL,
    34'799'618ULL,
    34'799'619ULL};

// The five files that the RAW and the Compressed directories of the NPS ntfs1
// image both hold.
constexpr std::array<std::wstring_view, 5> nps_files = {
    L"20076517123273.pdf", L"NISTSP800-88_rev1.pdf", L"NIST_logo.jpg",
    L"report02-3.pdf", L"logfile1.txt"};

// The threads of the scan that the file nps-ntfs1-scan-threads keeps: the last
// scan of the test, which uses several.
constexpr unsigned nps_scan_threads = 4;

}  // namespace

void CorpusProbes::Require(bool ok, std::string_view what) {
  if (!ok) {
    throw std::runtime_error(std::string(what));
  }
}

void CorpusProbes::OpenRootDir(Record& dir) {
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  Require(dir.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)),
          "the root record does not parse");
  Require(dir.ParseAttrs(), "the root attributes do not parse");
}

void CorpusProbes::OpenSubDir(Record& dir, std::wstring_view name) {
  const std::optional<IndexEntry> entry = dir.FindSubEntry(name);
  Require(entry.has_value(), "a directory is missing");
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  Require(dir.ParseFileRecord(entry->GetFileReference()),
          "a directory record does not parse");
  Require(dir.ParseAttrs(), "a directory has bad attributes");
}

void CorpusProbes::OpenDirPath(Record& dir,
                               std::initializer_list<std::wstring_view> parts) {
  OpenRootDir(dir);
  for (const std::wstring_view part : parts) {
    OpenSubDir(dir, part);
  }
}

void CorpusProbes::ReadNpsDirectory(const Volume& volume,
                                    std::wstring_view dir_name) {
  Record dir(volume);
  OpenDirPath(dir, {dir_name});
  for (const std::wstring_view name : nps_files) {
    const std::optional<IndexEntry> entry = dir.FindSubEntry(name);
    Require(entry.has_value(), "a file is missing");

    Record file(volume);
    file.SetAttrMask(Attr::Mask::Data);
    Require(file.ParseFileRecord(entry->GetFileReference()),
            "a file record does not parse");
    Require(file.ParseAttrs(), "a file has bad attributes");

    const Attr::AttrBase<Cache::Strategy::NoCache>* const stream =
        file.FindStream({});
    Require(stream != nullptr, "a file has no data");
    std::vector<BYTE> data(stream->GetDataSize());
    Require(stream->ReadData(0, data) == data.size(), "a file is short");
  }
}

void CorpusProbes::ReadRepairStreams(const Volume& volume) {
  Record dir(volume);
  OpenDirPath(dir, {L"$Extend", L"$RmMetadata"});

  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"$Repair");
  Require(entry.has_value(), "$Repair is missing");

  Record file(volume);
  file.SetAttrMask(Attr::Mask::Data);
  Require(file.ParseFileRecord(entry->GetFileReference()),
          "the $Repair record does not parse");
  Require(file.ParseAttrs(), "the $Repair attributes do not parse");

  for (const std::wstring_view stream_name : {L"$Corrupt", L"$Verify"}) {
    const Attr::AttrBase<Cache::Strategy::NoCache>* const stream =
        file.FindStream(stream_name);
    Require(stream != nullptr, "a $Repair stream is missing");
    std::vector<BYTE> data(stream->GetDataSize());
    Require(stream->ReadData(0, data) == data.size(),
            "a $Repair stream is short");
  }
}

void CorpusProbes::ReadKeywordFile(const Volume& volume, ULONGLONG mft_record,
                                   std::wstring_view stream_name,
                                   ULONGLONG size) {
  Record record(volume);
  record.SetAttrMask(Attr::Mask::Data);
  Require(record.ParseFileRecord(mft_record), "a keyword record is missing");
  Require(record.ParseAttrs(), "a keyword record has bad attributes");

  const Attr::AttrBase<Cache::Strategy::NoCache>* const stream =
      record.FindStream(stream_name);
  Require(stream != nullptr, "a keyword stream is missing");
  Require(stream->GetDataSize() == size, "a keyword stream has a wrong size");

  std::vector<BYTE> data(size);
  Require(stream->ReadData(0, data) == size, "a keyword stream is short");
}

void CorpusProbes::ReadDeletedFile(const Volume& volume, ULONGLONG mft_record,
                                   std::wstring_view stream_name,
                                   ULONGLONG size) {
  Record record(volume);
  record.SetAttrMask(Attr::Mask::Data | Attr::Mask::StandardInformation);
  Require(record.ParseFileRecord(mft_record), "a deleted record is missing");
  Require(record.ParseAttrs(), "a deleted record has bad attributes");

  const Attr::AttrBase<Cache::Strategy::NoCache>* const stream =
      record.FindStream(stream_name);
  Require(stream != nullptr, "a deleted stream is missing");
  Require(stream->GetDataSize() == size, "a deleted stream has a wrong size");

  std::vector<BYTE> data(size);
  Require(stream->ReadData(0, data) == size, "a deleted stream is short");
}

void CorpusProbes::AutodetectPartition(const Opener& open) {
  const Volume volume(open({}));
  Require(volume.IsVolumeOK(), "the volume is not ok");

  Record root(volume);
  OpenRootDir(root);
  const std::optional<IndexEntry> entry = root.FindSubEntry(L"ntfs.txt");
  Require(entry.has_value(), "ntfs.txt is missing");

  Record file(volume);
  file.SetAttrMask(Attr::Mask::Data);
  Require(file.ParseFileRecord(entry->GetFileReference()),
          "the ntfs.txt record does not parse");
  Require(file.ParseAttrs(), "the ntfs.txt attributes do not parse");
  Require(file.FindStream({}) != nullptr, "ntfs.txt has no data");
}

void CorpusProbes::KeywordSearch(const Opener& open) {
  const Volume volume(open("dftt-kwsrch"));
  Require(volume.IsVolumeOK(), "the volume is not ok");

  ReadKeywordFile(volume, 27, {}, kw_resident_size);
  ReadKeywordFile(volume, 29, L"here", kw_resident_size);
  ReadKeywordFile(volume, 30, L"there", kw_resident_size);
  ReadKeywordFile(volume, 33, {}, kw_non_resident_size);
  ReadKeywordFile(volume, 35, {}, kw_fragmented_size);
  ReadKeywordFile(volume, 37, L"here", kw_non_resident_size);
  ReadKeywordFile(volume, 38, L"there", kw_non_resident_size);

  const VolumeOptions options{.include_deleted = true};
  const Volume deleted_volume(open("dftt-kwsrch-deleted"), options);
  Require(deleted_volume.IsVolumeOK(), "the deleted volume is not ok");
  ReadKeywordFile(deleted_volume, kw_deleted_record, {}, kw_resident_size);

  Record slack(volume);
  slack.SetAttrMask(Attr::Mask::Data);
  Require(slack.ParseFileRecord(kw_slack_record),
          "the slack record is missing");
  Require(slack.ParseAttrs(), "the slack record has bad attributes");
  const Attr::AttrBase<Cache::Strategy::NoCache>* const stream =
      slack.FindStream({});
  Require(stream != nullptr, "the slack file has no data");
  Require(stream->GetDataSize() == kw_non_resident_size,
          "the slack file has a wrong size");
  std::vector<BYTE> data(kw_non_resident_size);
  Require(stream->ReadData(0, data) == kw_non_resident_size,
          "the slack file is short");
}

void CorpusProbes::Undelete(const Opener& open) {
  const VolumeOptions options{.include_deleted = true};
  const Volume volume(open({}), options);
  Require(volume.IsVolumeOK(), "the volume is not ok");

  ReadDeletedFile(volume, 37, {}, 101);
  ReadDeletedFile(volume, 31, {}, 780);
  ReadDeletedFile(volume, 32, {}, 3801);
  ReadDeletedFile(volume, 32, L"ADS", 1234);
  ReadDeletedFile(volume, 29, {}, 1584);
  ReadDeletedFile(volume, 30, {}, 3873);
  ReadDeletedFile(volume, 36, {}, 1715);
  ReadDeletedFile(volume, 35, {}, 2027);
  ReadDeletedFile(volume, 38, {}, 1005);

  // The two deleted directories themselves.
  Record dir1(volume);
  Require(dir1.ParseFileRecord(33), "the first deleted directory is missing");
  Record dir2(volume);
  Require(dir2.ParseFileRecord(34), "the second deleted directory is missing");
}

void CorpusProbes::FragmentedMft(const Opener& open) {
  const Volume volume(open({}));
  Require(volume.IsVolumeOK(), "the volume is not ok");

  for (const ULONGLONG record_num : fragmented_mft_records) {
    Record record(volume);
    Require(record.ParseFileRecord(record_num), "a $MFT record is missing");
  }
}

void CorpusProbes::LastAccess(const Opener& open) {
  const Volume volume(open({}));
  Require(volume.IsVolumeOK(), "the volume is not ok");

  Record dir(volume);
  OpenDirPath(dir, {L"test"});
  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"1.txt");
  Require(entry.has_value(), "1.txt is missing");

  Record file(volume);
  file.SetAttrMask(Attr::Mask::StandardInformation);
  Require(file.ParseFileRecord(entry->GetFileReference()),
          "the 1.txt record does not parse");
  Require(file.ParseAttrs(), "the 1.txt attributes do not parse");

  ReadRepairStreams(volume);
}

void CorpusProbes::SiVsFn(const Opener& open) {
  const Volume volume(open({}));
  Require(volume.IsVolumeOK(), "the volume is not ok");

  Record dir(volume);
  OpenDirPath(dir, {L"test"});
  const std::optional<IndexEntry> entry = dir.FindSubEntry(L"test.txt");
  Require(entry.has_value(), "test.txt is missing");

  Record file(volume);
  file.SetAttrMask(Attr::Mask::StandardInformation | Attr::Mask::FileName);
  Require(file.ParseFileRecord(entry->GetFileReference()),
          "the test.txt record does not parse");
  Require(file.ParseAttrs(), "the test.txt attributes do not parse");
}

void CorpusProbes::TwoFileTimes(const Opener& open) {
  const Volume volume(open({}));
  Require(volume.IsVolumeOK(), "the volume is not ok");

  const auto read_times = [&volume](std::wstring_view dir_name,
                                    std::wstring_view file_name) {
    Record dir(volume);
    OpenDirPath(dir, {dir_name});
    const std::optional<IndexEntry> entry = dir.FindSubEntry(file_name);
    Require(entry.has_value(), "a file is missing");

    Record file(volume);
    file.SetAttrMask(Attr::Mask::StandardInformation);
    Require(file.ParseFileRecord(entry->GetFileReference()),
            "a file record does not parse");
    Require(file.ParseAttrs(), "a file has bad attributes");
  };
  read_times(L"test_dir_2", L"file_2_1.txt");
  read_times(L"test_dir", L"file_1.txt");
}

void CorpusProbes::Ptrn(const Opener& open) {
  const Volume volume(open({}));
  Require(volume.IsVolumeOK(), "the volume is not ok");

  Record root(volume);
  OpenRootDir(root);
  Require(root.FindSubEntry(L"2.txt").has_value(), "2.txt is missing");

  ReadRepairStreams(volume);
}

void CorpusProbes::RamSlack(const Opener& open) {
  const Volume volume(open({}));
  Require(volume.IsVolumeOK(), "the volume is not ok");

  Record root(volume);
  OpenRootDir(root);
  Require(root.FindSubEntry(L"1.txt").has_value(), "1.txt is missing");

  for (const LONGLONG address : {ram_slack_address, cluster_slack_address}) {
    LARGE_INTEGER slack_addr{.QuadPart = address};
    Require(volume.Read(slack_addr, slack_probe_bytes).has_value(),
            "the slack is not readable");
  }

  ReadRepairStreams(volume);
}

void CorpusProbes::NpsRaw(const Opener& open) {
  const Volume volume(open({}), VolumeOptions{});
  Require(volume.IsVolumeOK(), "the volume is not ok");
  ReadNpsDirectory(volume, L"RAW");
}

void CorpusProbes::NpsCompressed(const Opener& open) {
  const Volume volume(open({}), VolumeOptions{});
  Require(volume.IsVolumeOK(), "the volume is not ok");
  ReadNpsDirectory(volume, L"Compressed");
}

void CorpusProbes::NpsScanThreads(const Opener& open) {
  const VolumeOptions options{.include_deleted = true, .recover_errors = true};
  const Volume volume(open({}), options);
  Require(volume.IsVolumeOK(), "the volume is not ok");

  Mft::MftTree::ScanOptions scan;
  scan.threads = nps_scan_threads;
  const Mft::MftTree tree(volume, scan);
  Require(!tree.Entries().empty(), "the scan found nothing");
}

void CorpusProbes::TwoMegabyteClusters(const Opener& open) {
  const Volume volume(open({}));
  Require(volume.IsVolumeOK(), "the volume is not ok");

  Record root(volume);
  OpenRootDir(root);
  root.TraverseSubEntries([](const IndexEntryView&, void*) {}, nullptr);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
