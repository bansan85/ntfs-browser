#include "external-mft-comparison.h"

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

#include "corpus-test-support.h"
#include "external-mft-support.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Whether two names of one record link to their parent alike. A strict scan
// drops the damaged parent of a name, so when strict_left is true the name may
// be linked to its parent only on the right.
bool SameParentLink(const Mft::MftTree::Name& left,
                    const Mft::MftTree::Name& right, bool strict_left) {
  const bool only_on_right =
      strict_left && !left.parent_valid && right.parent_valid;
  return left.parent_valid == right.parent_valid || only_on_right;
}

// Whether two names of one record agree on everything a name holds.
bool SameName(const Mft::MftTree::Name& left, const Mft::MftTree::Name& right,
              bool strict_left) {
  const auto fields = [](const Mft::MftTree::Name& name) {
    return std::tie(name.name, name.parent_record, name.parent_sequence,
                    name.dos_only);
  };
  return fields(left) == fields(right) &&
         SameParentLink(left, right, strict_left);
}

// Index of the first name two entries of the same name count disagree on.
std::optional<std::size_t> FirstDifferentName(const Mft::MftTree::Entry& left,
                                              const Mft::MftTree::Entry& right,
                                              bool strict_left) {
  for (std::size_t index = 0; index < left.names.size(); index++) {
    if (!SameName(left.names.at(index), right.names.at(index), strict_left)) {
      return index;
    }
  }
  return std::nullopt;
}

// Whether two entries carry the same four times, as ticks.
bool SameEntryTimes(const Mft::MftTree::Entry& left,
                    const Mft::MftTree::Entry& right) {
  return FileTimeToTicks(left.create_time) ==
             FileTimeToTicks(right.create_time) &&
         FileTimeToTicks(left.write_time) ==
             FileTimeToTicks(right.write_time) &&
         FileTimeToTicks(left.change_time) ==
             FileTimeToTicks(right.change_time) &&
         FileTimeToTicks(left.access_time) ==
             FileTimeToTicks(right.access_time);
}

// The attribute flags of an entry.
auto AttributeFlags(const Mft::MftTree::Entry& entry) {
  return std::tie(entry.read_only, entry.hidden, entry.system, entry.archive,
                  entry.compressed, entry.encrypted, entry.sparse);
}

// The counters of a scan.
auto ScanCounters(const Mft::MftTree::ScanStats& stats) {
  return std::tie(stats.slots, stats.in_use, stats.deleted, stats.extensions,
                  stats.unreadable, stats.skipped, stats.damaged,
                  stats.unreachable, stats.complete, stats.data_intact,
                  stats.data_reallocated, stats.data_unknown, stats.data_bad);
}

// Whether entry has a name, other than a DOS alias, whose path is the path of
// line.
bool HasListedName(const Mft::MftTree& tree, const Mft::MftTree::Entry& entry,
                   const FlsLine& line) {
  for (std::size_t index = 0; index < entry.names.size(); index++) {
    if (!entry.names.at(index).dos_only &&
        ListingPath(tree.GetPath(line.record, index)) == line.path) {
      return true;
    }
  }
  return false;
}

// Notes each field of line that entry, the record it names, disagrees with.
void CompareLineFields(const Mft::MftTree::Entry& entry, const FlsLine& line,
                       const std::string& where, ListingReport& report) {
  const std::array<std::uint64_t, 4> times = {
      EntryUtcSeconds(entry.write_time), EntryUtcSeconds(entry.access_time),
      EntryUtcSeconds(entry.change_time), EntryUtcSeconds(entry.create_time)};
  if (times != line.times) {
    report.Note(where + "times");
  }
  if (line.stream.empty() && entry.directory != (line.kind == "d/d")) {
    report.Note(where + "kind");
  }
  if (line.stream.empty() && line.kind == "r/r" && line.attribute_type == 128 &&
      entry.size != line.size) {
    report.Note(where + "size " + std::to_string(entry.size) + " vs " +
                std::to_string(line.size));
  }
}

// Compares one listing line with the tree, and counts it when its name exists.
void CompareLine(const Mft::MftTree& tree, const FlsLine& line,
                 ListingReport& report) {
  const std::string where =
      std::to_string(line.record) + " " + line.path + ": ";

  const Mft::MftTree::Entry* entry = tree.Find(line.record);
  if (entry == nullptr) {
    report.Note(where + "no entry");
    return;
  }
  if (!HasListedName(tree, *entry, line)) {
    report.Note(where + "no such name");
    return;
  }
  report.checked++;
  CompareLineFields(*entry, line, where, report);
}

// Notes each name of the tree, but the root's and the DOS aliases, that no
// listing line mentions.
void NoteUnlistedNames(
    const Mft::MftTree& tree,
    const std::set<std::pair<ULONGLONG, std::string>>& listed,
    ListingReport& report) {
  const ULONGLONG root = static_cast<ULONGLONG>(Mft::Idx::Root);
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    if (entry.record == root) {
      continue;
    }
    for (std::size_t index = 0; index < entry.names.size(); index++) {
      if (!entry.names.at(index).dos_only &&
          !listed.contains(
              {entry.record, ListingPath(tree.GetPath(entry.record, index))})) {
        report.Note(std::to_string(entry.record) + ": allocated but unlisted");
      }
    }
  }
}

}  // namespace

std::string FirstDifference(const Mft::MftTree::Entry& left,
                            const Mft::MftTree::Entry& right,
                            bool strict_left) {
  if (left.record != right.record) {
    return "record";
  }
  if (left.sequence != right.sequence) {
    return "sequence";
  }
  if (std::tie(left.in_use, left.directory) !=
      std::tie(right.in_use, right.directory)) {
    return "flags";
  }
  if (left.names.size() != right.names.size()) {
    return "name count";
  }
  if (const auto name = FirstDifferentName(left, right, strict_left)) {
    return "name " + std::to_string(*name);
  }
  if (std::tie(left.size, left.allocated_size) !=
      std::tie(right.size, right.allocated_size)) {
    return "size";
  }
  if (!SameEntryTimes(left, right)) {
    return "times";
  }
  if (AttributeFlags(left) != AttributeFlags(right)) {
    return "attribute flags";
  }
  if (left.data_state != right.data_state) {
    return "data state";
  }
  if (left.usn.has_value() != right.usn.has_value()) {
    return "usn";
  }
  return {};
}

std::string FirstDifference(const Mft::MftTree& left,
                            const Mft::MftTree& right) {
  if (ScanCounters(left.Stats()) != ScanCounters(right.Stats())) {
    return "stats";
  }
  if (left.Entries().size() != right.Entries().size()) {
    return "entry count";
  }
  for (std::size_t index = 0; index < left.Entries().size(); index++) {
    const std::string field =
        FirstDifference(left.Entries().at(index), right.Entries().at(index));
    if (!field.empty()) {
      return "record " + std::to_string(left.Entries().at(index).record) +
             ": " + field;
    }
  }
  return {};
}

void ListingReport::Note(const std::string& message) {
  constexpr std::size_t max_notes = 8;
  if (differences.size() < max_notes) {
    differences.push_back(message);
  }
}

ListingReport CompareWithListing(const Mft::MftTree& tree,
                                 const std::vector<FlsLine>& lines) {
  ListingReport report;
  std::set<std::pair<ULONGLONG, std::string>> listed;
  for (const FlsLine& line : lines) {
    if (line.kind == "V/V") {
      continue;
    }
    listed.emplace(line.record, line.path);
    CompareLine(tree, line, report);
  }
  NoteUnlistedNames(tree, listed, report);
  return report;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
