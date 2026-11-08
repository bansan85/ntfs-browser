#pragma once

#include <ntfs-browser/win-types.h>

#include <functional>
#include <initializer_list>
#include <memory>
#include <string_view>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The reads that the corpus tests of NTFSLibTests/unit-tests/corpus make on a
// small forensic volume, without their checks. Running one on a SparseImage
// gives the trace that the test records from the real image. A probe MUST keep
// the same reads, in the same order, as its test.
class CorpusProbes {
 public:
  // Opens the volume that the trace called trace_name stands for. An empty
  // name means the only trace of the probe.
  using Opener =
      std::function<std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>(
          std::string_view trace_name)>;

  // The probe of one trace: opens the volumes it needs and reads them.
  using Probe = void (*)(const Opener& open);

  // DFTT test #10: the root directory, ntfs.txt and its data stream. The
  // trace is dftt-autodetect-part1, -part2 or -part3, all with this probe.
  static void AutodetectPartition(const Opener& open);

  // DFTT test #3: the keyword files, the deleted one and the slack file. The
  // traces are dftt-kwsrch and dftt-kwsrch-deleted.
  static void KeywordSearch(const Opener& open);

  // DFTT test #7: the deleted files and directories. The trace is dftt-undel.
  static void Undelete(const Opener& open);

  // The records of a very fragmented $MFT. The trace is ntfs-fragmented-mft.
  static void FragmentedMft(const Opener& open);

  // /test/1.txt of ntfs-lastaccess.raw, then the $Repair streams. The trace is
  // ntfs-lastaccess.
  static void LastAccess(const Opener& open);

  // /test/test.txt of ntfs-si-vs-fn.raw. The trace is ntfs-si-vs-fn.
  static void SiVsFn(const Opener& open);

  // Two files of ntfs.raw. The trace is ntfs.
  static void TwoFileTimes(const Opener& open);

  // /2.txt of ntfs-ptrn.raw, then the $Repair streams. The trace is ntfs-ptrn.
  static void Ptrn(const Opener& open);

  // /1.txt of ntfs-ramslack.raw, its RAM slack and cluster slack, then the
  // $Repair streams. The trace is ntfs-ramslack.
  static void RamSlack(const Opener& open);

  // The root directory of ntfs-2m.raw, which has 2 MiB clusters. The trace is
  // ntfs-2m.
  static void TwoMegabyteClusters(const Opener& open);

 private:
  using Volume = NTFS_BROWSER_NAMESPACE::NtfsVolume<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>;
  using Record = NTFS_BROWSER_NAMESPACE::Io::FileRecord<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>;

  // Throws std::runtime_error with what when ok is false: the test would stop
  // at that point, and the trace with it.
  static void Require(bool ok, std::string_view what);

  // Parses the root directory into dir, ready for FindSubEntry().
  static void OpenRootDir(Record& dir);

  // Looks up name in dir and parses dir again as that subdirectory.
  static void OpenSubDir(Record& dir, std::wstring_view name);

  // Walks dir down from the root, one name at a time.
  static void OpenDirPath(Record& dir,
                          std::initializer_list<std::wstring_view> parts);

  // Reads the $Corrupt and $Verify streams of $Extend\$RmMetadata\$Repair.
  static void ReadRepairStreams(const Volume& volume);

  // Parses the record of a search-term file and reads its stream.
  static void ReadKeywordFile(const Volume& volume, ULONGLONG mft_record,
                              std::wstring_view stream_name, ULONGLONG size);

  // Parses the record of a deleted file and reads its stream.
  static void ReadDeletedFile(const Volume& volume, ULONGLONG mft_record,
                              std::wstring_view stream_name, ULONGLONG size);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
