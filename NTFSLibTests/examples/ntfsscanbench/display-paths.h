#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "mft/tree.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

// Bytes that two threads must not both write to. The size of a cache line on
// the processors the tool runs on; std::hardware_destructive_interference_size
// says the same, but the compilers warn when a header uses it.
inline constexpr size_t cache_line_bytes = 64;

// The name MftTree::GetPath() goes through when it is given none: the first
// non-DOS name with a valid parent, else the first non-DOS name, else the
// first name. Empty when entry has no name.
std::optional<size_t>
    PrimaryName(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry);

// Counts the names that are hard links, leaving out DOS 8.3 aliases.
size_t CountHardLinks(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry);

// Builds the paths of the listing, the ones MftTree::GetPath() gives, in
// UTF-8. Converting the whole path of each file costs more than the rest of
// its line, so the path of each directory is converted once and kept: a file's
// path is its directory's, plus its own name.
//
// Prepare() runs on one thread, and Display() on any thread: the listing is
// formatted by several. A directory is built in Prepare(), for every entry
// whose Display() will need it, and Display() only reads. The tree is not
// thread safe, and its GetPath() keeps a memo that it changes: every call goes
// through a lock here. Prepare() MUST have returned for an entry, and what it
// changed MUST be visible to the thread that calls Display() for it, before
// that call. Prepare() of another entry MAY run at the same time: it changes
// what no Display() of an entry already prepared reads.
class DisplayPaths {
 public:
  using Entry = NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry;

  // The path of a directory through its primary name, and the record where
  // its chain of valid parent references breaks, if it does.
  struct Directory {
    std::optional<ULONGLONG> lost;
    std::string path;
    // Built by the tree, which remembers its own memo of it: see Keep().
    bool from_tree = false;
  };

  // What one thread needs to build a path: strings it reuses. Each thread
  // writes its own all the time, so two of them MUST NOT share a cache line.
  struct alignas(cache_line_bytes) Scratch {
    // The path of the entry being printed, and of its other hard links.
    std::string path;
    std::string other;
    // A directory path that was not kept, valid until the next call.
    Directory unkept;
  };

  explicit DisplayPaths(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree);

  // Builds the directory paths that Display() of entry, and of each of its
  // hard links, will need.
  void Prepare(const Entry& entry);

  // Replaces out with the path of entry through entry.names[name_index], or
  // through its primary name when name_index is empty, as the listing shows
  // it.
  void Display(const Entry& entry, std::optional<size_t> name_index,
               std::string& out, Scratch& scratch) const;

 private:
  // How the path of one name of an entry is built.
  struct Route {
    enum class Kind : std::uint8_t {
      // The entry has no such name.
      NoName,
      // The entry is a directory and the name its primary one: the path is the
      // one its files reuse.
      Own,
      // A file under a directory the scan found: that directory's path, and
      // the name.
      Through,
      // A name whose parent reference is not valid: the name alone, relative
      // to the record the reference names.
      Lost,
      // A name filed in the root: a backslash, and the name.
      UnderRoot,
      // Any other: the tree walks the chain of parents.
      Walk
    };
    Kind kind = Kind::NoName;
    // The directory whose path Through builds on.
    const Entry* directory = nullptr;
    // The name, in the entry's names.
    size_t index = 0;
  };

  // Where the chain of parents that KeepByChain() walks ends.
  struct ChainEnd {
    // The kept directory the chain meets, if any.
    const Directory* known = nullptr;
    // The record where the chain of valid parent references breaks, if it
    // does.
    std::optional<ULONGLONG> lost;
  };

  // Value of slot_of_ for a directory not built yet, and for one built and
  // not kept, which Display() builds again each time it is asked for.
  static constexpr uint32_t no_slot = 0;
  static constexpr uint32_t unkept_slot = UINT32_MAX;

  // Levels of a chain KeepByChain() walks at most before it leaves the path to
  // the tree: a path of that depth is rarely short enough to be kept anyway.
  static constexpr size_t max_chain_levels = 256;

  [[nodiscard]] Route RouteOf(const Entry& entry,
                              std::optional<size_t> name_index) const;
  [[nodiscard]] const Entry* ThroughDirectory(
      const Entry& entry,
      const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name) const;
  void Build(const Entry& directory, Directory& out) const;
  void Keep(const Route& route);
  bool KeepByChain(const Entry& directory);
  bool WalkChain(const Entry& directory, ChainEnd& end);
  void StoreChain(const ChainEnd& end);
  void Store(const Entry& entry, Directory&& directory);
  [[nodiscard]] size_t IndexOf(const Entry& entry) const;
  const Directory& DirectoryOf(const Entry& directory, Scratch& scratch) const;

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree_;
  // Guards every call to the tree that changes its memo.
  mutable std::mutex tree_mutex_;
  // The first entry of the tree: the index of an entry is its distance to it.
  const Entry* first_;
  // The slot of the directory at each entry index: no_slot, unkept_slot, or
  // one more than the index in directories_.
  std::vector<uint32_t> slot_of_;
  // The kept paths, in the order they were built. It never grows: a reader
  // MAY hold a path while Keep() adds another.
  std::unique_ptr<Directory[]> directories_;
  size_t capacity_{0};
  size_t kept_count_{0};
  // The directories KeepByChain() walks, from the one asked for upwards.
  std::vector<const Entry*> chain_;
  // Bytes of path directories_ holds.
  size_t kept_bytes_{0};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
