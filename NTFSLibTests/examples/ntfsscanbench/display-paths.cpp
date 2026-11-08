#include "display-paths.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/text-convert.h>

using namespace NTFS_BROWSER_NAMESPACE;

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// The record of the root directory, where a path starts.
constexpr ULONGLONG root_record = static_cast<ULONGLONG>(Mft::Idx::Root);

// Bytes of directory path DisplayPaths keeps at most, in all. A chain of
// directories that runs deep has paths that grow with the depth, so keeping
// each one costs memory that grows with its square. 256 MiB holds the million
// directories of a system volume listed with --deleted, 118 bytes of path each
// on average. A path that does not fit is built each time it is asked for,
// which takes a lock that the threads that format share: the listing then
// slows down as they grow in number. A 32-bit process keeps 64 MiB.
constexpr size_t max_kept_path_bytes =
    sizeof(void*) > 4 ? size_t{256} << 20 : size_t{64} << 20;

// Bytes a directory path can have and still be kept. A longer path is built
// each time it is asked for, which costs no more than printing it.
constexpr size_t max_kept_path_length = 4096;

// Appends the "<lost #N>\" that starts a path relative to the lost record N.
void AppendLostPrefix(std::string& out, const std::optional<ULONGLONG>& lost) {
  if (lost) {
    out += "<lost #";
    out += std::to_string(*lost);
    out += ">\\";
  }
}

}  // namespace

std::optional<size_t>
    PrimaryName(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  std::optional<size_t> fallback;
  for (size_t i = 0; i < entry.names.size(); i++) {
    // i < entry.names.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name = entry.names[i];
    if (name.dos_only) {
      continue;
    }
    if (name.parent_valid) {
      return i;
    }
    if (!fallback) {
      fallback = i;
    }
  }
  if (fallback) {
    return fallback;
  }
  return entry.names.empty() ? std::nullopt : std::optional<size_t>{0};
}

size_t
    CountHardLinks(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  size_t links = 0;
  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name : entry.names) {
    if (!name.dos_only) {
      links++;
    }
  }
  return links;
}

DisplayPaths::DisplayPaths(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree)
    : tree_(tree),
      first_(tree.Entries().data()),
      slot_of_(tree.Entries().size(), no_slot) {
  // A path is kept for at most this many directories: the entries that are
  // directories, and no more than a slot number holds.
  const auto directories = static_cast<size_t>(
      std::ranges::count_if(tree.Entries(), &Entry::directory));
  capacity_ = (std::min)(directories, size_t{unkept_slot} - 1);
  directories_ = std::make_unique<Directory[]>(capacity_);
}

void DisplayPaths::Prepare(const Entry& entry) {
  Keep(RouteOf(entry, std::nullopt));
  // A lone non-DOS name is the one the path above went through.
  if (CountHardLinks(entry) > 1) {
    for (size_t i = 0; i < entry.names.size(); i++) {
      // i < entry.names.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      if (!entry.names[i].dos_only) {
        Keep(RouteOf(entry, i));
      }
    }
  }
}

void DisplayPaths::Display(const Entry& entry, std::optional<size_t> name_index,
                           std::string& out, Scratch& scratch) const {
  out.clear();
  const Route route = RouteOf(entry, name_index);
  switch (route.kind) {
    case Route::Kind::NoName:
      out = "<no name>";
      return;
    case Route::Kind::Own: {
      const Directory& own = DirectoryOf(entry, scratch);
      if (own.path.empty()) {
        out = "<no name>";
        return;
      }
      AppendLostPrefix(out, own.lost);
      out += own.path;
      return;
    }
    case Route::Kind::Through: {
      const Directory& directory = DirectoryOf(*route.directory, scratch);
      AppendLostPrefix(out, directory.lost);
      out += directory.path;
      out += '\\';
      // route.index was checked against entry.names by RouteOf().
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      Tool::DeviceAccess::AppendAsUtf8(out, entry.names[route.index].name);
      return;
    }
    case Route::Kind::Lost: {
      // route.index was checked against entry.names by RouteOf().
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name =
          entry.names[route.index];
      if (name.name.empty()) {
        out = "<no name>";
        return;
      }
      AppendLostPrefix(out, name.parent_record);
      Tool::DeviceAccess::AppendAsUtf8(out, name.name);
      return;
    }
    case Route::Kind::UnderRoot:
      out += '\\';
      // route.index was checked against entry.names by RouteOf().
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      Tool::DeviceAccess::AppendAsUtf8(out, entry.names[route.index].name);
      return;
    case Route::Kind::Walk:
      break;
  }

  std::optional<ULONGLONG> lost;
  std::wstring path;
  {
    const std::scoped_lock lock{tree_mutex_};
    path = tree_.GetPath(entry.record, route.index, &lost);
  }
  if (path.empty()) {
    out = "<no name>";
    return;
  }
  AppendLostPrefix(out, lost);
  Tool::DeviceAccess::AppendAsUtf8(out, path);
}

// Decides how the path of entry through name_index, or through its primary
// name when that is empty, is built. It reads nothing that Prepare() changes.
DisplayPaths::Route
    DisplayPaths::RouteOf(const Entry& entry,
                          std::optional<size_t> name_index) const {
  const std::optional<size_t> primary = PrimaryName(entry);
  const std::optional<size_t> index = name_index ? name_index : primary;
  if (!index || *index >= entry.names.size()) {
    return {};
  }
  if (entry.directory && index == primary) {
    return {.kind = Route::Kind::Own, .directory = &entry, .index = *index};
  }

  // *index < entry.names.size() was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name = entry.names[*index];
  if (const Entry* parent = ThroughDirectory(entry, name)) {
    return {.kind = Route::Kind::Through, .directory = parent, .index = *index};
  }
  // The tree gives "\\" to the root, whatever its names say.
  if (entry.record != root_record) {
    if (!name.parent_valid) {
      return {.kind = Route::Kind::Lost, .directory = nullptr, .index = *index};
    }
    if (name.parent_record == root_record) {
      return {.kind = Route::Kind::UnderRoot,
              .directory = nullptr,
              .index = *index};
    }
  }
  return {.kind = Route::Kind::Walk, .directory = nullptr, .index = *index};
}

// The directory whose kept path a file's name builds on: its valid parent,
// other than the root, when the scan found that parent and it has a name. That
// path comes from the parent's primary name through GetPath(), as it does for
// its files. Null for a directory, the root, and any other name.
const DisplayPaths::Entry* DisplayPaths::ThroughDirectory(
    const Entry& entry,
    const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name) const {
  if (entry.directory || entry.record == root_record || !name.parent_valid ||
      name.parent_record == root_record) {
    return nullptr;
  }
  const Entry* parent = tree_.Find(name.parent_record);
  if (parent == nullptr || parent->names.empty()) {
    return nullptr;
  }
  return parent;
}

// Builds the path of directory, through the tree.
void DisplayPaths::Build(const Entry& directory, Directory& out) const {
  out.path.clear();
  out.lost.reset();
  std::wstring path;
  {
    const std::scoped_lock lock{tree_mutex_};
    path = tree_.GetPath(directory.record, &out.lost);
  }
  Tool::DeviceAccess::AppendAsUtf8(out.path, path);
}

// Builds and keeps the path of the directory route needs, if any.
void DisplayPaths::Keep(const Route& route) {
  if (route.kind != Route::Kind::Own && route.kind != Route::Kind::Through) {
    return;
  }
  // directory belongs to the tree, so its index is below its size.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  if (slot_of_[IndexOf(*route.directory)] != no_slot) {
    return;
  }
  if (KeepByChain(*route.directory)) {
    return;
  }
  Directory directory;
  Build(*route.directory, directory);
  directory.from_tree = true;
  Store(*route.directory, std::move(directory));
}

// Builds and keeps the path of directory, and of the ancestors it goes
// through, from the primary names of the chain, as the tree does. False,
// and nothing changed, for the cases it leaves to the tree: the root, a
// chain that loops, one that runs past max_chain_levels without meeting a
// directory that is kept, and one that meets a directory the tree built. The
// tree remembers more than it gives for a chain that loops: every directory
// of the loop gets a path of its own, which a name joined to a parent's path
// would not equal. So it answers for all that touches such a chain, in the
// order of the listing. Its memo of paths stays empty for all the others,
// which costs a copy of each path less.
bool DisplayPaths::KeepByChain(const Entry& directory) {
  if (directory.record == root_record) {
    return false;
  }
  ChainEnd end;
  if (!WalkChain(directory, end)) {
    return false;
  }
  StoreChain(end);
  return true;
}

// Fills chain_ with the directories from directory upwards, up to the first
// that is filed in the root, has a broken parent reference, or has a kept
// parent, and says in end where it stopped. False for a chain that loops, that
// runs past max_chain_levels, or that meets a directory the tree built.
bool DisplayPaths::WalkChain(const Entry& directory, ChainEnd& end) {
  chain_.clear();
  const Entry* current = &directory;
  while (true) {
    const std::optional<size_t> primary = PrimaryName(*current);
    if (!primary || chain_.size() >= max_chain_levels ||
        std::ranges::find(chain_, current) != chain_.end()) {
      return false;
    }
    chain_.push_back(current);
    // PrimaryName() gave an index of the names.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name =
        current->names[*primary];
    if (!name.parent_valid) {
      end.lost = name.parent_record;
      return true;
    }
    if (name.parent_record == root_record) {
      return true;
    }
    const Entry* parent = tree_.Find(name.parent_record);
    if (parent == nullptr || !PrimaryName(*parent)) {
      end.lost = name.parent_record;
      return true;
    }
    // parent belongs to the tree, so its index is below its size.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const uint32_t slot = slot_of_[IndexOf(*parent)];
    if (slot != no_slot && slot != unkept_slot) {
      // slot - 1 is the index Store() put the directory at.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      end.known = &directories_[slot - 1];
      if (end.known->from_tree) {
        return false;
      }
      end.lost = end.known->lost;
      return true;
    }
    current = parent;
  }
}

// Keeps the path of each directory of chain_, from the top down, as the tree
// joins them, starting from the path end names.
void DisplayPaths::StoreChain(const ChainEnd& end) {
  Directory path;
  path.lost = end.lost;
  if (end.known != nullptr) {
    path.path = end.known->path;
  }
  for (const Entry* const item : chain_ | std::views::reverse) {
    if (!path.path.empty() || !path.lost) {
      path.path += '\\';
    }
    // The chain holds entries that PrimaryName() gave a name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    Tool::DeviceAccess::AppendAsUtf8(path.path,
                                     item->names[*PrimaryName(*item)].name);
    Store(*item, Directory{path});
  }
}

// Keeps entry's path, unless it is too long or the budget has no room for it:
// then Display() builds it each time it is asked for. An entry that has a
// slot keeps it: a thread MAY be reading it.
void DisplayPaths::Store(const Entry& entry, Directory&& directory) {
  // entry belongs to the tree, so its index is below its size.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  uint32_t& slot = slot_of_[IndexOf(entry)];
  if (slot != no_slot) {
    return;
  }
  if (directory.path.size() > max_kept_path_length ||
      kept_bytes_ + directory.path.size() > max_kept_path_bytes ||
      kept_count_ >= capacity_) {
    slot = unkept_slot;
    return;
  }
  kept_bytes_ += directory.path.size();
  // kept_count_ < capacity_ was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  directories_[kept_count_] = std::move(directory);
  slot = static_cast<uint32_t>(++kept_count_);
}

// The index of an entry of the tree.
size_t DisplayPaths::IndexOf(const Entry& entry) const {
  return static_cast<size_t>(&entry - first_);
}

// The path of the directory, as Keep() built it, or built now when it did
// not keep it.
const DisplayPaths::Directory&
    DisplayPaths::DirectoryOf(const Entry& directory, Scratch& scratch) const {
  // directory belongs to the tree, so its index is below its size.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const uint32_t slot = slot_of_[IndexOf(directory)];
  if (slot != no_slot && slot != unkept_slot) {
    // slot - 1 is the index Store() put the directory at.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return directories_[slot - 1];
  }
  Build(directory, scratch.unkept);
  return scratch.unkept;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
