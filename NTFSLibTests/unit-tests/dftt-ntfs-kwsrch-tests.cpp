#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "corpus-test-support.h"

using NtfsBrowser::AttrBase;
using NtfsBrowser::FileRecord;
namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::VolumeOptions;

namespace {

// DFTT test #3 ("NTFS Keyword Search #1", http://dftt.sf.net): an 8 MB NTFS
// file system holding ten ASCII search terms, each placed to exercise one
// combination of resident/non-resident, allocated/deleted, and plain/
// alternate-data-stream content.
const std::filesystem::path dftt_image =
    NtfsBrowserTests::DfttDir() / "3-kwsrch-ntfs" / "ntfs-img-kw-1.dd";

// One DFTT test #3 search-term case, addressed by its own MFT record number
// instead of by path (index.html gives the search term and the file it
// lives in; the record numbers themselves come from walking the image).
struct KeywordFile {
  ULONGLONG mft_record;
  std::wstring_view stream_name;  // {} for the unnamed $DATA stream
  std::string_view keyword;
  ULONGLONG size;
  bool deleted;
  bool is_directory;
};

// Sizes index.html gives the files: resident, non-resident, and non-resident
// across fragmented clusters.
constexpr ULONGLONG resident_size = 120;
constexpr ULONGLONG non_resident_size = 2000;
constexpr ULONGLONG fragmented_size = 2600;

// The MFT record of each search-term case, from walking the image.
constexpr KeywordFile resident_alloc{27,    {},   "r-alloc", resident_size,
                                     false, false};
constexpr KeywordFile resident_file_ads{29,    L"here", "r-fads", resident_size,
                                        false, false};
constexpr KeywordFile resident_dir_ads{30,    L"there", "r-dads", resident_size,
                                       false, true};
constexpr KeywordFile non_resident_alloc{
    33, {}, "n-alloc", non_resident_size, false, false};
constexpr KeywordFile non_resident_frag{35,    {},   "n-frag", fragmented_size,
                                        false, false};
constexpr KeywordFile non_resident_file_ads{
    37, L"here", "n-fads", non_resident_size, false, false};
constexpr KeywordFile non_resident_dir_ads{
    38, L"there", "n-dads", non_resident_size, false, true};
constexpr KeywordFile resident_unalloc{34,   {},   "r-unalloc", resident_size,
                                       true, false};

// The non-resident file whose search term sits in its slack space.
constexpr ULONGLONG slack_record = 36;

// Parses one MFT record from `volume` and checks that the requested stream
// has the size and deletion/directory state index.html documents, and that
// its content contains the DFTT search term.
void CheckReadsKeywordFile(const NtfsVolume<Cache::Strategy::NoCache>& volume,
                           const KeywordFile& file) {
  FileRecord record(volume);
  record.SetAttrMask(Attr::Mask::Data);
  REQUIRE(record.ParseFileRecord(file.mft_record));
  CHECK(record.IsDeleted() == file.deleted);
  REQUIRE(record.ParseAttrs());
  CHECK(record.IsDirectory() == file.is_directory);

  const AttrBase<Cache::Strategy::NoCache>* stream =
      record.FindStream(file.stream_name);
  REQUIRE(stream != nullptr);
  REQUIRE(stream->GetDataSize() == file.size);

  std::vector<BYTE> data(file.size);
  REQUIRE(stream->ReadData(0, data) == file.size);
  const std::string_view content(reinterpret_cast<const char*>(data.data()),
                                 data.size());
  CHECK_THAT(std::string(content),
             Catch::Matchers::ContainsSubstring(std::string(file.keyword)));
}

}  // namespace

TEST_CASE("Reads DFTT test #3 (NTFS Keyword Search) files",
          "[dftt][integration]") {
  NtfsBrowserTests::RequireCorpusImage(dftt_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NtfsBrowserTests::OpenBareVolumeImage(dftt_image));
  REQUIRE(volume.IsVolumeOK());

  CheckReadsKeywordFile(volume, resident_alloc);
  CheckReadsKeywordFile(volume, resident_file_ads);
  CheckReadsKeywordFile(volume, resident_dir_ads);
  CheckReadsKeywordFile(volume, non_resident_alloc);
  CheckReadsKeywordFile(volume, non_resident_frag);
  CheckReadsKeywordFile(volume, non_resident_file_ads);
  CheckReadsKeywordFile(volume, non_resident_dir_ads);

  // Resident unallocated (deleted) file: ParseAttrs() only sees it with
  // include_deleted on.
  VolumeOptions options;
  options.include_deleted = true;
  const NtfsVolume<Cache::Strategy::NoCache> del_volume(
      NtfsBrowserTests::OpenBareVolumeImage(dftt_image), options);
  REQUIRE(del_volume.IsVolumeOK());
  CheckReadsKeywordFile(del_volume, resident_unalloc);

  // Non-resident allocated file: index.html places its search term in this
  // file's slack space, past its logical size. ReadData()/GetDataSize()
  // expose only the file's own content, so the term MUST NOT be found there.
  FileRecord slack(volume);
  slack.SetAttrMask(Attr::Mask::Data);
  REQUIRE(slack.ParseFileRecord(slack_record));
  REQUIRE(slack.ParseAttrs());
  const AttrBase<Cache::Strategy::NoCache>* slack_data = slack.FindStream({});
  REQUIRE(slack_data != nullptr);
  REQUIRE(slack_data->GetDataSize() == non_resident_size);
  std::vector<BYTE> data(non_resident_size);
  REQUIRE(slack_data->ReadData(0, data) == non_resident_size);
  const std::string_view content(reinterpret_cast<const char*>(data.data()),
                                 data.size());
  CHECK_THAT(std::string(content),
             !Catch::Matchers::ContainsSubstring("n-slack"));
}
