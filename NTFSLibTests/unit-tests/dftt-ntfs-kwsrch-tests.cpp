#include <filesystem>
#include <string>
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
    NtfsBrowserTests::kDfttDir / "3-kwsrch-ntfs" / "ntfs-img-kw-1.dd";

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

  NtfsVolume<Strategy::NO_CACHE> volume(kDfttImage.wstring());
  REQUIRE(volume.IsVolumeOK());

  // Resident allocated file.
  CheckReadsKeywordFile(volume, {27, {}, "r-alloc", 120, false, false});
  // Resident alternate data stream in an allocated file.
  CheckReadsKeywordFile(volume, {29, L"here", "r-fads", 120, false, false});
  // Resident alternate data stream in an allocated directory.
  CheckReadsKeywordFile(volume, {30, L"there", "r-dads", 120, false, true});
  // Non-resident allocated file.
  CheckReadsKeywordFile(volume, {33, {}, "n-alloc", 2000, false, false});
  // Non-resident allocated file, crossing fragmented clusters.
  CheckReadsKeywordFile(volume, {35, {}, "n-frag", 2600, false, false});
  // Non-resident alternate data stream in an allocated file.
  CheckReadsKeywordFile(volume, {37, L"here", "n-fads", 2000, false, false});
  // Non-resident alternate data stream in an allocated directory.
  CheckReadsKeywordFile(volume, {38, L"there", "n-dads", 2000, false, true});

  // Resident unallocated (deleted) file: ParseAttrs() only sees it with
  // include_deleted on.
  VolumeOptions options;
  options.include_deleted = true;
  NtfsVolume<Strategy::NO_CACHE> del_volume(kDfttImage.wstring(), options);
  REQUIRE(del_volume.IsVolumeOK());
  CheckReadsKeywordFile(del_volume, {34, {}, "r-unalloc", 120, true, false});

  // Non-resident allocated file: index.html places its search term in this
  // file's slack space, past its logical size. ReadData()/GetDataSize()
  // expose only the file's own content, so the term MUST NOT be found there.
  FileRecord slack(volume);
  slack.SetAttrMask(Mask::DATA);
  REQUIRE(slack.ParseFileRecord(36));
  REQUIRE(slack.ParseAttrs());
  const AttrBase<Strategy::NO_CACHE>* slack_data = slack.FindStream({});
  REQUIRE(slack_data != nullptr);
  REQUIRE(slack_data->GetDataSize() == 2000);
  std::vector<BYTE> data(2000);
  REQUIRE(slack_data->ReadData(0, data) == 2000);
  const std::string_view content(reinterpret_cast<const char*>(data.data()),
                                 data.size());
  CHECK(content.find("n-slack") == std::string_view::npos);
}
