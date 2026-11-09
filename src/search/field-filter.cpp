#include "search/field-filter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace {

// Every bit that Attribute:: names. A bit outside it is a mistake of the
// caller: no engine reports one.
constexpr std::uint32_t known_attribute_bits =
    Attribute::read_only | Attribute::hidden | Attribute::system |
    Attribute::directory | Attribute::archive | Attribute::device |
    Attribute::normal | Attribute::temporary | Attribute::sparse |
    Attribute::reparse_point | Attribute::compressed | Attribute::offline |
    Attribute::not_content_indexed | Attribute::encrypted;

// The bits that the MftList engine reports: the flags of $STANDARD_INFORMATION
// that the record tree keeps. The directory engines report all the known ones.
constexpr std::uint32_t mft_list_attribute_bits =
    Attribute::read_only | Attribute::hidden | Attribute::system |
    Attribute::directory | Attribute::archive | Attribute::sparse |
    Attribute::compressed | Attribute::encrypted;

// The bit of value in a set of standings or of data states.
constexpr std::uint32_t BitOf(std::uint8_t value) noexcept {
  return std::uint32_t{1} << value;
}

// The name of a field that only the MftList engine gives and that filter
// tests. Null when there is none.
const char* MftListOnlyField(const Filter& filter) {
  if (filter.link_count.IsSet()) {
    return "link_count";
  }
  if (!filter.clusters.empty()) {
    return "clusters";
  }
  return nullptr;
}

// The name of a field that only the directory engines give and that filter
// tests. Null when there is none.
const char* DirectoryOnlyField(const Filter& filter) {
  const std::uint32_t outside = ~mft_list_attribute_bits;
  if (((filter.attributes_all | filter.attributes_none) & outside) != 0) {
    return "the attribute bits device, normal, temporary, reparse_point, "
           "offline or not_content_indexed";
  }
  const auto& standing = filter.standing_in;
  if (std::ranges::any_of(standing, [](Standing value) {
        return value == Standing::Reused || value == Standing::Unreadable;
      })) {
    return "the standing Reused or Unreadable";
  }
  return nullptr;
}

// Whether a data state other than NotChecked is asked for.
bool NeedsDataState(const Filter& filter) {
  return std::ranges::any_of(filter.data_state_in, [](DataState value) {
    return value != DataState::NotChecked;
  });
}

// Whether the standings that pass include only deleted ones.
bool OnlyDeletedStandings(const Filter& filter) {
  if (filter.standing_in.empty()) {
    return false;
  }
  return std::ranges::none_of(filter.standing_in, [](Standing value) {
    return value == Standing::Live || value == Standing::Unreadable;
  });
}

// Logs an error naming field when is_empty, and returns is_empty.
bool Reject(bool is_empty, std::string_view field) {
  if (is_empty) {
    Log::Error("The filter on {} has a minimum above its maximum", field);
  }
  return is_empty;
}

// Logs the first test of filter that can match nothing, and tells whether there
// was one.
bool HasImpossibleTest(const Filter& filter) {
  if (Reject(filter.size.IsEmpty(), "size") ||
      Reject(filter.allocated_size.IsEmpty(), "allocated_size") ||
      Reject(filter.created.IsEmpty(), "created") ||
      Reject(filter.modified.IsEmpty(), "modified") ||
      Reject(filter.mft_changed.IsEmpty(), "mft_changed") ||
      Reject(filter.accessed.IsEmpty(), "accessed") ||
      Reject(filter.record.IsEmpty(), "record") ||
      Reject(filter.link_count.IsEmpty(), "link_count")) {
    return true;
  }
  return std::ranges::any_of(filter.clusters, [](const ClusterTest& test) {
    return Reject(test.at_most && *test.at_most < test.at_least, "clusters");
  });
}

// Whether every cluster test names a condition that exists.
bool ClusterConditionsExist(const Filter& filter) {
  return std::ranges::all_of(filter.clusters, [](const ClusterTest& test) {
    return static_cast<std::size_t>(test.condition) < cluster_condition_count;
  });
}

// Whether the filter sets at least one test.
bool AnyTestSet(const Filter& filter) {
  return filter.size.IsSet() || filter.allocated_size.IsSet() ||
         filter.created.IsSet() || filter.modified.IsSet() ||
         filter.mft_changed.IsSet() || filter.accessed.IsSet() ||
         filter.attributes_all != 0 || filter.attributes_none != 0 ||
         filter.is_directory.has_value() ||
         filter.deleted != DeletedFilter::Any || !filter.standing_in.empty() ||
         filter.record.IsSet() || filter.parent_record.has_value() ||
         filter.link_count.IsSet() || !filter.data_state_in.empty() ||
         !filter.clusters.empty();
}

// Whether time passes a range on a time. An unset time never does.
bool TimePasses(const Range<Date>& range, Date time) {
  return !range.IsSet() || (time.IsSet() && range.Contains(time));
}

}  // namespace

FilterNeeds NeedsOf(const Filter& filter) {
  return {.mft_list_only = MftListOnlyField(filter) != nullptr,
          .directory_only = DirectoryOnlyField(filter) != nullptr,
          .deleted_data_state =
              NeedsDataState(filter) || !filter.clusters.empty(),
          .deleted_entries_only =
              filter.deleted == DeletedFilter::OnlyDeleted ||
              OnlyDeletedStandings(filter) || NeedsDataState(filter) ||
              !filter.clusters.empty()};
}

std::optional<FieldFilter> FieldFilter::Make(const Options& options,
                                             EngineKind engine) {
  const Filter& filter = options.filter;
  if (HasImpossibleTest(filter)) {
    return std::nullopt;
  }
  if (((filter.attributes_all | filter.attributes_none) &
       ~known_attribute_bits) != 0) {
    Log::Error("The filter on the attributes names a bit that does not exist");
    return std::nullopt;
  }
  if (!ClusterConditionsExist(filter)) {
    Log::Error("The filter on clusters names a condition that does not exist");
    return std::nullopt;
  }
  if (engine == EngineKind::MftList) {
    if (const char* field = DirectoryOnlyField(filter)) {
      Log::Error("The filter tests {}, which the MftList engine does not give",
                 field);
      return std::nullopt;
    }
  } else if (const char* field = MftListOnlyField(filter)) {
    Log::Error("The filter tests {}, which only the MftList engine gives",
               field);
    return std::nullopt;
  }
  if (NeedsOf(filter).deleted_entries_only && !options.include_deleted) {
    Log::Warn(
        "The filter keeps deleted entries only, and the scan does not "
        "list them: nothing will match");
  }
  return FieldFilter(filter);
}

FieldFilter::FieldFilter(const Filter& filter)
    : filter_(filter), active_(AnyTestSet(filter)) {
  for (const Standing value : filter.standing_in) {
    standing_bits_ |= BitOf(static_cast<std::uint8_t>(value));
  }
  for (const DataState value : filter.data_state_in) {
    data_state_bits_ |= BitOf(static_cast<std::uint8_t>(value));
  }
}

bool FieldFilter::Matches(const Entry& entry) const {
  if (!active_) {
    return true;
  }
  const Filter& filter = filter_;
  if (!filter.record.Contains(entry.record) ||
      (filter.parent_record && *filter.parent_record != entry.parent_record) ||
      (filter.is_directory && *filter.is_directory != entry.is_directory)) {
    return false;
  }
  return MatchesNumbers(entry) && MatchesState(entry);
}

bool FieldFilter::MatchesNumbers(const Entry& entry) const {
  const Filter& filter = filter_;
  if ((entry.attributes & filter.attributes_all) != filter.attributes_all ||
      (entry.attributes & filter.attributes_none) != 0) {
    return false;
  }
  return filter.size.Contains(entry.size) &&
         filter.allocated_size.Contains(entry.allocated_size) &&
         filter.link_count.Contains(entry.link_count) &&
         TimePasses(filter.created, entry.created) &&
         TimePasses(filter.modified, entry.modified) &&
         TimePasses(filter.mft_changed, entry.mft_changed) &&
         TimePasses(filter.accessed, entry.accessed);
}

bool FieldFilter::MatchesState(const Entry& entry) const {
  const Filter& filter = filter_;
  switch (filter.deleted) {
    case DeletedFilter::Any:
      break;
    case DeletedFilter::OnlyLive:
      if (entry.standing != Standing::Live) {
        return false;
      }
      break;
    case DeletedFilter::OnlyDeleted:
      if (!entry.IsDeleted()) {
        return false;
      }
      break;
  }
  if (!filter.standing_in.empty() &&
      (standing_bits_ & BitOf(static_cast<std::uint8_t>(entry.standing))) ==
          0) {
    return false;
  }
  if (!filter.data_state_in.empty() &&
      (data_state_bits_ & BitOf(static_cast<std::uint8_t>(entry.data_state))) ==
          0) {
    return false;
  }
  if (filter.clusters.empty()) {
    return true;
  }
  if (entry.clusters == nullptr) {
    return false;
  }
  return std::ranges::all_of(filter.clusters, [&entry](
                                                  const ClusterTest& test) {
    const std::uint64_t count = entry.clusters->Count(test.condition);
    return count >= test.at_least && (!test.at_most || count <= *test.at_most);
  });
}

}  // namespace NTFS_BROWSER_NAMESPACE
