#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "corpus-test-support.h"

using NtfsBrowser::AttrBase;
using NtfsBrowser::FileRecord;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;

namespace
{

// DFTT test #3 ("NTFS Keyword Search #1", http://dftt.sf.net): an 8 MB NTFS
// file system holding ten ASCII search terms, each placed to exercise one
// combination of resident/non-resident, allocated/deleted, and plain/
// alternate-data-stream content.
const std::filesystem::path kDfttImage =
    NtfsBrowserTests::DfttDir() / "3-kwsrch-ntfs" / "ntfs-img-kw-1.dd";

// One DFTT test #3 search-term case, addressed by its own MFT record number
// instead of by path (index.html gives the search term and the file it
// lives in; the record numbers themselves come from walking the image).
struct KeywordFile
{
  ULONGLONG mft_record;
  std::wstring_view stream_name;  // {} for the unnamed $DATA stream
  std::string_view keyword;
  ULONGLONG size;
  bool deleted;
  bool is_directory;
};

// Sizes index.html gives the files: resident, non-resident, and non-resident
// across fragmented clusters.
constexpr ULONGLONG kResidentSize = 120;
constexpr ULONGLONG kNonResidentSize = 2000;
constexpr ULONGLONG kFragmentedSize = 2600;

// The MFT record of each search-term case, from walking the image.
constexpr KeywordFile kResidentAlloc{27,    {},   "r-alloc", kResidentSize,
                                     false, false};
constexpr KeywordFile kResidentFileAds{29,    L"here", "r-fads", kResidentSize,
                                       false, false};
constexpr KeywordFile kResidentDirAds{30,    L"there", "r-dads", kResidentSize,
                                      false, true};
constexpr KeywordFile kNonResidentAlloc{
    33, {}, "n-alloc", kNonResidentSize, false, false};
constexpr KeywordFile kNonResidentFrag{35,    {},   "n-frag", kFragmentedSize,
                                       false, false};
constexpr KeywordFile kNonResidentFileAds{
    37, L"here", "n-fads", kNonResidentSize, false, false};
constexpr KeywordFile kNonResidentDirAds{
    38, L"there", "n-dads", kNonResidentSize, false, true};
constexpr KeywordFile kResidentUnalloc{34,   {},   "r-unalloc", kResidentSize,
                                       true, false};

// The non-resident file whose search term sits in its slack space.
constexpr ULONGLONG kSlackRecord = 36;

// Parses one MFT record from `volume` and checks that the requested stream
// has the size and deletion/directory state index.html documents, and that
// its content contains the DFTT search term.
void CheckReadsKeywordFile(const NtfsVolume<Strategy::NO_CACHE>& volume,
                           const KeywordFile& file)
{
  FileRecord record(volume);
  record.SetAttrMask(Mask::DATA);
  REQUIRE(record.ParseFileRecord(file.mft_record));
  CHECK(record.IsDeleted() == file.deleted);
  REQUIRE(record.ParseAttrs());
  CHECK(record.IsDirectory() == file.is_directory);

  const AttrBase<Strategy::NO_CACHE>* stream =
      record.FindStream(file.stream_name);
  REQUIRE(stream != nullptr);
  REQUIRE(stream->GetDataSize() == file.size);

  std::vector<BYTE> data(file.size);
  REQUIRE(stream->ReadData(0, data) == file.size);
  const std::string_view content(reinterpret_cast<const char*>(data.data()),
                                 data.size());
  CHECK(content.find(file.keyword) != std::string_view::npos);
}

}  // namespace

TEST_CASE("Reads DFTT test #3 (NTFS Keyword Search) files",
          "[dftt][integration]")
{
  NtfsBrowserTests::RequireCorpusImage(kDfttImage);

  NtfsVolume<Strategy::NO_CACHE> volume(
      NtfsBrowserTests::OpenBareVolumeImage(kDfttImage));
  REQUIRE(volume.IsVolumeOK());

  CheckReadsKeywordFile(volume, kResidentAlloc);
  CheckReadsKeywordFile(volume, kResidentFileAds);
  CheckReadsKeywordFile(volume, kResidentDirAds);
  CheckReadsKeywordFile(volume, kNonResidentAlloc);
  CheckReadsKeywordFile(volume, kNonResidentFrag);
  CheckReadsKeywordFile(volume, kNonResidentFileAds);
  CheckReadsKeywordFile(volume, kNonResidentDirAds);

  // Resident unallocated (deleted) file: ParseAttrs() only sees it with
  // include_deleted on.
  VolumeOptions options;
  options.include_deleted = true;
  NtfsVolume<Strategy::NO_CACHE> del_volume(
      NtfsBrowserTests::OpenBareVolumeImage(kDfttImage), options);
  REQUIRE(del_volume.IsVolumeOK());
  CheckReadsKeywordFile(del_volume, kResidentUnalloc);

  // Non-resident allocated file: index.html places its search term in this
  // file's slack space, past its logical size. ReadData()/GetDataSize()
  // expose only the file's own content, so the term MUST NOT be found there.
  FileRecord slack(volume);
  slack.SetAttrMask(Mask::DATA);
  REQUIRE(slack.ParseFileRecord(kSlackRecord));
  REQUIRE(slack.ParseAttrs());
  const AttrBase<Strategy::NO_CACHE>* slack_data = slack.FindStream({});
  REQUIRE(slack_data != nullptr);
  REQUIRE(slack_data->GetDataSize() == kNonResidentSize);
  std::vector<BYTE> data(kNonResidentSize);
  REQUIRE(slack_data->ReadData(0, data) == kNonResidentSize);
  const std::string_view content(reinterpret_cast<const char*>(data.data()),
                                 data.size());
  CHECK(content.find("n-slack") == std::string_view::npos);
}
