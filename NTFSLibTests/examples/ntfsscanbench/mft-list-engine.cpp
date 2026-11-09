// Lists every file the $MFT records, with its characteristics. Paths come
// from each record's own $FILE_NAME parent reference, not from directory
// indexes, so files of a directory whose index is lost still show up. With
// --recover, the change journal ($UsnJrnl) adds what it knows of each file, and
// lists the files only it names. With --clusters, each freed file, and each
// file that is damaged or salvaged, gets a line that counts its clusters by
// condition: intact, rewritten by another file, and so on.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/tool/device-access/bench-session.h>
#include <ntfs-browser/tool/device-access/device-path.h>
#include <ntfs-browser/tool/device-access/line-buffer.h>
#include <ntfs-browser/tool/device-access/local-time.h>
#include <ntfs-browser/tool/device-access/ordered-chunks.h>
#include <ntfs-browser/tool/device-access/output.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/tool/device-access/tool-support.h>

#include "alloc/cluster-account.h"
#include "arguments.h"
#include "bench/bench.h"
#include "cache/strategy.h"
#include "display-paths.h"
#include "engine.h"
#include "log/log.h"
#include "mft/idx.h"
#include "mft/tree.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "usn/types.h"
#include "volume-options.h"
#include "win-types.h"

namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

#ifdef _WIN32
  #include <cwctype>

#else
  #include <ntfs-browser/tool/device-access/posix-device-disk-reader.h>
#endif

using namespace NTFS_BROWSER_NAMESPACE;
using ToolDeviceAccess::Output;

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

#ifndef _WIN32
// Bits in a DWORD: where a FILETIME's high half starts.
constexpr unsigned dword_bits = 32;
// Year a FILETIME of zero names.
constexpr unsigned filetime_epoch_year = 1601;
#endif

// Width of every column before the path, separators included, so an extra
// hard link's "=" line lines up under the path it follows.
constexpr int path_column = 63;

// Widths of the record, sequence, DEL, size and hard link columns: wide enough
// for the values a volume holds, and with the 16 of the date and the 6 of the
// attributes, plus a space after each, they add up to path_column.
constexpr size_t record_width = 10;
constexpr size_t sequence_width = 5;
constexpr size_t state_width = 3;
constexpr size_t size_width = 14;
constexpr size_t links_width = 2;

// Entries in a chunk of the listing, the unit that a thread formats. A chunk
// takes about half a millisecond to format, so handing one over costs next to
// nothing, and a listing of a few thousand entries still spreads.
constexpr size_t entries_per_chunk = 1024;

// Threads that format the listing at most when --threads does not say. The
// calling thread builds the directory paths and appends the chunks while the
// others format. Measured on volumes of 1.3 and 4.7 million entries, three
// threads in all end the listing as soon as four, six or eight do, and take
// a quarter less CPU than four: the calling thread is then the one that the
// others wait for.
constexpr size_t max_auto_format_threads = 3;

// Entries a listing needs before the formatting takes threads of its own when
// --threads does not say: a smaller listing is done before they have started.
constexpr size_t min_auto_threaded_entries = 16384;

// Chunks a formatting thread may be ahead of the one written, per thread, and
// at the least. A chunk is about 200 KiB of text, so this bounds the text held
// apart to a few MiB.
constexpr size_t chunks_ahead_per_thread = 3;
constexpr size_t min_chunks_ahead = 4;

// Converts to UTF-8, which the console is switched to on Windows, so every
// name prints whatever the active code page or locale.
std::string ToUtf8(std::wstring_view text) {
  return ToolDeviceAccess::WideToUtf8(text);
}

// The last write time of an entry in the host's local zone. A time the
// platform cannot show reads as all zeros.
ToolDeviceAccess::LocalTime LocalWriteTime(const FILETIME& write_time) {
#ifdef _WIN32
  // The library already converts the time to local on Windows.
  SYSTEMTIME local{};
  FileTimeToSystemTime(&write_time, &local);
  return {.year = local.wYear,
          .month = local.wMonth,
          .day = local.wDay,
          .hour = local.wHour,
          .minute = local.wMinute};
#else
  // The library leaves the time in UTC off Windows.
  const ULONGLONG ticks =
      (static_cast<ULONGLONG>(write_time.dwHighDateTime) << dword_bits) |
      write_time.dwLowDateTime;
  // A record with no $STANDARD_INFORMATION keeps a time of zero, which
  // Windows shows as it is, so the zone does not move it.
  if (ticks == 0) {
    return {.year = filetime_epoch_year, .month = 1, .day = 1};
  }
  return ToolDeviceAccess::UtcTicksToLocalTime(ticks).value_or(
      ToolDeviceAccess::LocalTime{});
#endif
}

// The letter of one attribute flag when it is set, '-' when it is not.
char FlagLetter(bool set, char letter) { return set ? letter : '-'; }

// The six attribute flags as letters, each shown when its flag is set.
std::string
    AttributeColumn(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  return {FlagLetter(entry.read_only, 'R'), FlagLetter(entry.hidden, 'H'),
          FlagLetter(entry.system, 'S'),    FlagLetter(entry.compressed, 'C'),
          FlagLetter(entry.encrypted, 'E'), FlagLetter(entry.sparse, 'P')};
}

// Prints the columns of entry, then its path.
void PrintMainLine(ToolDeviceAccess::LineBuffer& out,
                   const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                   const std::string& path) {
  const ToolDeviceAccess::LocalTime local_time =
      LocalWriteTime(entry.write_time);

  out.AppendNumber(entry.record, record_width);
  out.Append(" ");
  out.AppendNumber(entry.sequence, sequence_width);
  out.Append(" ");
  out.AppendLeft(entry.in_use ? "" : "DEL", state_width);
  out.Append(" ");
  if (entry.directory) {
    out.AppendRight("<DIR>", size_width);
  } else {
    out.AppendNumber(entry.size, size_width);
  }
  out.Append(" ");
  out.AppendNumber(local_time.year, 4, '0');
  out.Append("-");
  out.AppendNumber(local_time.month, 2, '0');
  out.Append("-");
  out.AppendNumber(local_time.day, 2, '0');
  out.Append(" ");
  out.AppendNumber(local_time.hour, 2, '0');
  out.Append(":");
  out.AppendNumber(local_time.minute, 2, '0');
  out.Append(" ");
  out.Append(AttributeColumn(entry));
  out.Append(" ");
  out.AppendNumber(CountHardLinks(entry), links_width);
  out.Append(" ");
  out.Append(path);
  out.EndLine();
}

// Prints one "=" line for each hard link whose path differs from path.
void PrintOtherLinks(ToolDeviceAccess::LineBuffer& out,
                     const DisplayPaths& paths,
                     const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                     const std::string& path, DisplayPaths::Scratch& scratch) {
  for (size_t i = 0; i < entry.names.size(); i++) {
    // i < entry.names.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (entry.names[i].dos_only) {
      continue;
    }
    paths.Display(entry, i, scratch.other, scratch);
    if (scratch.other != path) {
      out.PrintLine("{:{}}= {}", "", path_column, scratch.other);
    }
  }
}

// One reason bit of a journal record, and the name the listing gives it.
struct ReasonName {
  Usn::Reason bit;
  std::string_view name;
};

// The reasons a journal record can give, in bit order.
constexpr std::array<ReasonName, 24> reason_names{{
    {Usn::Reason::DataOverwrite, "DataOverwrite"},
    {Usn::Reason::DataExtend, "DataExtend"},
    {Usn::Reason::DataTruncation, "DataTruncation"},
    {Usn::Reason::NamedDataOverwrite, "NamedDataOverwrite"},
    {Usn::Reason::NamedDataExtend, "NamedDataExtend"},
    {Usn::Reason::NamedDataTruncation, "NamedDataTruncation"},
    {Usn::Reason::FileCreate, "FileCreate"},
    {Usn::Reason::FileDelete, "FileDelete"},
    {Usn::Reason::EaChange, "EaChange"},
    {Usn::Reason::SecurityChange, "SecurityChange"},
    {Usn::Reason::RenameOldName, "RenameOldName"},
    {Usn::Reason::RenameNewName, "RenameNewName"},
    {Usn::Reason::IndexableChange, "IndexableChange"},
    {Usn::Reason::BasicInfoChange, "BasicInfoChange"},
    {Usn::Reason::HardLinkChange, "HardLinkChange"},
    {Usn::Reason::CompressionChange, "CompressionChange"},
    {Usn::Reason::EncryptionChange, "EncryptionChange"},
    {Usn::Reason::ObjectIdChange, "ObjectIdChange"},
    {Usn::Reason::ReparsePointChange, "ReparsePointChange"},
    {Usn::Reason::StreamChange, "StreamChange"},
    {Usn::Reason::TransactedChange, "TransactedChange"},
    {Usn::Reason::IntegrityChange, "IntegrityChange"},
    {Usn::Reason::DesiredStorageClassChange, "DesiredStorageClassChange"},
    {Usn::Reason::Close, "Close"},
}};

// The names of the reason bits set in reasons, joined by '|'.
std::string ReasonText(DWORD reasons) {
  std::string text;
  for (const ReasonName& reason : reason_names) {
    if ((reasons & static_cast<DWORD>(reason.bit)) != 0) {
      text += text.empty() ? "" : "|";
      text += reason.name;
    }
  }
  return text.empty() ? "-" : text;
}

// A time of the journal as "YYYY-MM-DD hh:mm".
std::string TimeText(const FILETIME& time) {
  const ToolDeviceAccess::LocalTime local = LocalWriteTime(time);
  char text[32]{};
  snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", local.year,
           local.month, local.day, local.hour, local.minute);
  return text;
}

// Prints what the journal knows of an entry, under its listing lines.
void PrintJournalLine(
    ToolDeviceAccess::LineBuffer& out,
    const NTFS_BROWSER_NAMESPACE::Mft::MftTree::UsnInfo& info) {
  out.Print("{:{}}~ {} {}", "", path_column, TimeText(info.last_time),
            ReasonText(info.last_reason));
  if (info.delete_time) {
    out.Print(", deleted {}", TimeText(*info.delete_time));
  }
  out.PrintLine(" ({} records)", info.record_count);
}

// The non-zero counters of tally, as "name count, name count".
std::string TallyText(const Alloc::ClusterTally& tally) {
  std::string text;
  for (size_t i = 0; i < Alloc::cluster_condition_count; i++) {
    const auto condition = static_cast<Alloc::ClusterCondition>(i);
    if (tally.Count(condition) == 0) {
      continue;
    }
    text += text.empty() ? "" : ", ";
    text += Alloc::ConditionName(condition);
    text += " " + std::to_string(tally.Count(condition));
  }
  return text.empty() ? "-" : text;
}

// What the scan counted of the clusters of one entry. account is null when the
// scan did not count clusters. cluster_size turns the cluster ranges of the map
// into byte ranges.
struct ClusterColumns {
  const Alloc::ClusterAccount* account = nullptr;
  ULONGLONG cluster_size = 0;
};

// Prints what the scan counted of an entry's clusters, and with the map, the
// byte ranges of each stream in file order.
void PrintClusterLines(ToolDeviceAccess::LineBuffer& out,
                       const Alloc::ClusterAccount& account,
                       ULONGLONG cluster_size) {
  out.PrintLine("{:{}}# clusters: {}", "", path_column,
                TallyText(account.tally));
  for (const Alloc::StreamClusterMap& map : account.streams) {
    out.Print("{:{}}# stream {}:", "", path_column,
              map.name.empty() ? std::string("(unnamed)") : ToUtf8(map.name));
    for (const Alloc::StreamByteRange& range :
         Alloc::ToByteRanges(map, cluster_size)) {
      out.Print(" {}..{} {}", range.offset, range.offset + range.length,
                Alloc::ConditionName(range.condition));
    }
    out.EndLine();
  }
}

// Prints one listing line for entry, then one line per other hard link, then
// what the journal knows of it, then what is known of its clusters. scratch is
// reused from one entry to the next.
void PrintEntry(ToolDeviceAccess::LineBuffer& out, const DisplayPaths& paths,
                const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                const ClusterColumns& clusters,
                DisplayPaths::Scratch& scratch) {
  paths.Display(entry, std::nullopt, scratch.path, scratch);
  PrintMainLine(out, entry, scratch.path);
  // A lone non-DOS name is the one the path above went through.
  if (CountHardLinks(entry) > 1) {
    PrintOtherLinks(out, paths, entry, scratch.path, scratch);
  }
  if (entry.usn) {
    PrintJournalLine(out, *entry.usn);
  }
  if (clusters.account != nullptr) {
    PrintClusterLines(out, *clusters.account, clusters.cluster_size);
  }
}

// The path of one file only the journal names, as shown in the listing.
std::string JournalDisplayPath(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                               size_t index) {
  std::optional<ULONGLONG> lost;
  const std::wstring path = tree.GetJournalPath(index, &lost);
  if (lost) {
    return "<lost #" + std::to_string(*lost) + ">\\" + ToUtf8(path);
  }
  return ToUtf8(path);
}

// How many threads format a listing of entry_count entries: requested when
// --threads gave a count. Otherwise one for a small listing, and for a big one
// the machine's, up to max_auto_format_threads. The formatting is all CPU, so
// the test the scan makes of its disk does not apply to it.
size_t FormatThreads(unsigned requested, size_t entry_count) {
  if (requested != 0) {
    return requested;
  }
  if (entry_count < min_auto_threaded_entries) {
    return 1;
  }
  return std::clamp<size_t>(std::thread::hardware_concurrency(), 1,
                            max_auto_format_threads);
}

// Prints the listing of every entry of tree, one chunk of entries_per_chunk at
// a time. The chunks are formatted on several threads, see FormatThreads(), and
// reach out in order, so the listing is the one of a single thread. counted
// says the scan counted clusters.
void PrintEntries(Output& out, const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                  bool counted, ULONGLONG cluster_size,
                  unsigned requested_threads) {
  const auto& entries = tree.Entries();
  const size_t chunk_count =
      (entries.size() + entries_per_chunk - 1) / entries_per_chunk;
  const size_t threads =
      (std::min)(FormatThreads(requested_threads, entries.size()), chunk_count);
  const size_t window =
      (std::max)(min_chunks_ahead, chunks_ahead_per_thread * threads);

  DisplayPaths paths(tree);

  // The text of the chunks in flight, each on a cache line of its own: a thread
  // changes the size of its text with every piece it appends.
  struct alignas(cache_line_bytes) ChunkText {
    ToolDeviceAccess::LineBuffer text;
  };

  std::vector<ChunkText> chunks(window);
  std::vector<DisplayPaths::Scratch> scratches(threads);
  // The entries of chunk, as an index range of entries.
  const auto first_of = [&entries](size_t chunk) {
    return (std::min)(chunk * entries_per_chunk, entries.size());
  };
  // Chunk c and chunk c + window are never held at once, so they share a slot.
  const auto slot_of = [window](size_t chunk) { return chunk % window; };

  ToolDeviceAccess::ChunkSteps steps;
  steps.prepare = [&](size_t chunk) {
    for (size_t i = first_of(chunk); i < first_of(chunk + 1); i++) {
      // i < entries.size() by first_of().
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      paths.Prepare(entries[i]);
    }
  };
  steps.make = [&](size_t chunk, size_t slot) {
    // slot_of() is below window, and slot below threads.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    ToolDeviceAccess::LineBuffer& text = chunks[slot_of(chunk)].text;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    DisplayPaths::Scratch& scratch = scratches[slot];
    for (size_t i = first_of(chunk); i < first_of(chunk + 1); i++) {
      // i < entries.size() by first_of().
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const auto& entry = entries[i];
      PrintEntry(
          text, paths, entry,
          {.account = counted ? tree.ClusterAccountOf(entry.record) : nullptr,
           .cluster_size = cluster_size},
          scratch);
    }
  };
  steps.consume = [&](size_t chunk) {
    // slot_of() is below window.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    ToolDeviceAccess::LineBuffer& text = chunks[slot_of(chunk)].text;
    out.AppendBlock(text.Text());
    text.Clear();
  };
  ToolDeviceAccess::RunOrderedChunks(
      {.chunk_count = chunk_count, .threads = threads, .window = window},
      steps);
}

// Prints the files only the journal names, after the listing.
void PrintJournalOnly(Output& out,
                      const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree) {
  const auto files = tree.JournalOnly();
  if (files.empty()) {
    return;
  }
  out.EndLine();
  out.PrintLine("Files only the journal names: {}", files.size());
  out.PrintLine("{:>10} {:>5} {:<5} {:<16} {}", "Record", "Seq", "",
                "Last event", "Path [reasons]");
  for (size_t i = 0; i < files.size(); i++) {
    // i < files.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto& file = files[i];
    out.PrintLine("{:>10} {:>5} {:<5} {:<16} {} [{}]", file.record,
                  file.sequence, file.directory ? "<DIR>" : "",
                  TimeText(file.usn.last_time), JournalDisplayPath(tree, i),
                  ReasonText(file.usn.all_reasons));
  }
}

#ifdef _WIN32

// Opens target as a drive letter ("c" or "c:"), or else as a device or
// image path.
std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>>
    OpenVolume(std::wstring_view target, const VolumeOptions& options) {
  // A lone letter, optionally followed by a colon, names a drive.
  const bool drive_letter =
      (target.size() == 1 || (target.size() == 2 && target.back() == L':')) &&
      iswalpha(target.front()) != 0;
  if (drive_letter) {
    return std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(
        target.front(), options);
  }
  return std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(target,
                                                                options);
}

#else

// Opens target, a block device or an image file, through a POSIX reader.
// Returns null after saying why when it cannot.
std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>>
    OpenVolume(std::wstring_view target, const VolumeOptions& options) {
  // The whole volume is listed, so the target must name no directory.
  const std::optional<ToolDeviceAccess::DeviceTarget> split =
      ToolDeviceAccess::SplitDeviceTarget(target);
  if (!split || !split->directory.empty()) {
    return nullptr;
  }

  auto reader = std::make_unique<ToolDeviceAccess::PosixDeviceDiskReader>();
  if (!reader->Open(ToolDeviceAccess::WideFromPath(split->device))) {
    const int error = reader->LastError();
    if (error == EACCES || error == EPERM) {
      fprintf(stderr, "Cannot read device %s: read access is needed\n",
              split->device.c_str());
    }
    return nullptr;
  }
  return std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(
      std::move(reader), options);
}

#endif  // _WIN32

// Shows the $MFT scan's progress on stderr, and ends the line when it is done.
// An update takes a write of stderr, which costs milliseconds when that is a
// file on a busy disk: the line is redrawn a few times a second, not once per
// call.
class ScanProgress {
 public:
  bool operator()(ULONGLONG done, ULONGLONG total) {
    const auto now = std::chrono::steady_clock::now();
    if (done != total && drawn_ && now - last_draw_ < redraw_interval) {
      return true;
    }
    drawn_ = true;
    last_draw_ = now;
    // One write per update: stderr is unbuffered, and fprintf() would write it
    // piece by piece.
    std::array<char, 80> line{};
    const int length = snprintf(
        line.data(), line.size(), "\rScanning $MFT: %llu / %llu%s",
        static_cast<unsigned long long>(done),
        static_cast<unsigned long long>(total), done == total ? "\n" : "");
    if (length > 0) {
      fwrite(line.data(), 1, static_cast<size_t>(length), stderr);
    }
    return true;
  }

 private:
  // Time between two redraws of the progress line: often enough to look alive.
  static constexpr std::chrono::milliseconds redraw_interval{100};

  std::chrono::steady_clock::time_point last_draw_;
  bool drawn_{false};
};

// Prints the scan totals that close the listing. clusters says the scan
// counted clusters.
void PrintStats(Output& out,
                const NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanStats& stats,
                bool clusters) {
  out.EndLine();
  out.PrintLine(
      "Record slots: {}, in use: {}, deleted: {} ({} with an overwritten "
      "attribute list), extensions: {} ({} reused by others)",
      stats.slots, stats.in_use, stats.deleted, stats.overwritten_lists,
      stats.extensions, stats.reused_extensions);
  out.PrintLine("Unreadable: {}, damaged: {}, not linked to the root: {}",
                stats.unreadable, stats.damaged, stats.unreachable);
  if (stats.usn_records != 0) {
    out.PrintLine(
        "Journal: {} records, {} files annotated, {} only in the journal, {} "
        "damaged{}",
        stats.usn_records, stats.usn_annotated, stats.usn_journal_only,
        stats.usn_damaged, stats.usn_truncated ? " (truncated)" : "");
  }
  if (clusters) {
    out.PrintLine("Clusters of {} files: {}", stats.cluster_accounts,
                  TallyText(stats.clusters));
  }
}

}  // namespace

int RunMftListEngine(
    const Arguments& args,
    const NTFS_BROWSER_NAMESPACE::VolumeOptions& volume_options,
    const RunPlan& plan, std::uint64_t& entries) {
  const std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>> volume =
      OpenVolume(args.target, volume_options);
  if (!volume || !volume->IsVolumeOK()) {
    fprintf(stderr, "Cannot open %s as an NTFS volume\n",
            ToUtf8(args.target).c_str());
    return -1;
  }
  if (NTFS_BROWSER_NAMESPACE::Bench::StopRequested()) {
    return 0;
  }

  NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanOptions scan_options;
  scan_options.progress = ScanProgress{};
  scan_options.threads = args.threads;
  scan_options.cluster_account = args.clusters;
  scan_options.verify_data = args.verify;
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::Mft::MftTree> owner(
      new NTFS_BROWSER_NAMESPACE::Mft::MftTree(*volume, scan_options));
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree = *owner;
  if (!plan.release) {
    // Never freed: the process ends with the listing, and freeing millions of
    // entries one by one would take seconds of it. A static keeps it
    // reachable.
    static const NTFS_BROWSER_NAMESPACE::Mft::MftTree* kept = nullptr;
    kept = owner.release();
  }
  entries = tree.Entries().size();

  // The scan was told to stop: there is no tree to print.
  if (NTFS_BROWSER_NAMESPACE::Bench::StopRequested()) {
    return 0;
  }

  // The writer thread is joined when out goes, inside the phase.
  const NTFS_BROWSER_NAMESPACE::Bench::PhaseScope print_phase(
      NTFS_BROWSER_NAMESPACE::Bench::Phase::Print);
  Output out(volume_options.tuning.output.flush_bytes, plan.sink);
  out.PrintLine("{:>10} {:>5} {:<3} {:>14} {:<16} {:<6} {:>2} {}", "Record",
                "Seq", "", "Size", "Last write", "Attrib", "Ln", "Path");
  const bool counted = args.clusters != Alloc::ClusterAccountLevel::Off ||
                       args.verify != Alloc::DataCheckMode::Off;
  PrintEntries(out, tree, counted, volume->GetClusterSize(), args.threads);
  PrintJournalOnly(out, tree);
  PrintStats(out, tree.Stats(), counted);

  return 0;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
