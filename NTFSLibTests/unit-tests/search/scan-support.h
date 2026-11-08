#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

namespace NTFS_BROWSER_NAMESPACE::Test::Search {

// An image written to a temp file, removed with the object.
class TempImage final {
 public:
  explicit TempImage(const std::vector<BYTE>& image);
  TempImage(const TempImage&) = delete;
  TempImage& operator=(const TempImage&) = delete;
  TempImage(TempImage&&) = delete;
  TempImage& operator=(TempImage&&) = delete;
  ~TempImage();

  // The scan target of the image, followed by inside when it is not empty.
  // Slashes separate: they work on every host.
  [[nodiscard]] std::u16string Target(std::u16string_view inside = {}) const;

  [[nodiscard]] const std::filesystem::path& Path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

// An Entry as a test keeps it: the views copied out.
struct Row {
  std::u16string name;
  std::u16string path;
  std::uint64_t record{0};
  std::uint16_t sequence{0};
  std::uint64_t parent_record{0};
  std::uint64_t size{0};
  std::uint64_t allocated_size{0};
  std::uint64_t created{0};
  std::uint64_t modified{0};
  std::uint64_t mft_changed{0};
  std::uint64_t accessed{0};
  std::uint32_t attributes{0};
  bool is_directory{false};
  NTFS_BROWSER_NAMESPACE::Search::Standing standing{
      NTFS_BROWSER_NAMESPACE::Search::Standing::Live};
  std::uint16_t link_count{0};
  NTFS_BROWSER_NAMESPACE::Search::DataState data_state{
      NTFS_BROWSER_NAMESPACE::Search::DataState::NotChecked};
  bool has_clusters{false};

  [[nodiscard]] bool operator==(const Row&) const = default;
};

// What a whole scan gave.
struct Outcome {
  std::vector<Row> rows;
  NTFS_BROWSER_NAMESPACE::Search::State state{
      NTFS_BROWSER_NAMESPACE::Search::State::NotStarted};
  std::size_t batches{0};
  // How many batches had last set.
  std::size_t last_batches{0};
  NTFS_BROWSER_NAMESPACE::Search::Progress progress;
};

// Runs a scan with options to its end, polling, and collects what it gave.
[[nodiscard]] Outcome
    RunScan(const NTFS_BROWSER_NAMESPACE::Search::Options& options);

// Polls scan until it is over, or until a long deadline, and returns its state.
// A scan that is not over by then is stuck and reports Running.
NTFS_BROWSER_NAMESPACE::Search::State
    PollToEnd(NTFS_BROWSER_NAMESPACE::Search::Scan& scan);

// The names of rows, as UTF-8, in order.
[[nodiscard]] std::vector<std::string> Names(const std::vector<Row>& rows);

// The folder tree that most tests scan. Under the root: Docs (record 16) and
// root.txt (20). Docs holds a.txt (17), Sub (19) and the deleted gone.txt
// (21). Sub holds c.txt (18).
[[nodiscard]] std::vector<BYTE> TreeImage();

// The volume of the tree nodes below, for the MftList engine. Under the root:
// Docs (record 16, a folder), one.txt (17, also named two.txt in Docs) and
// LongName.txt (18, with the DOS alias LONGNA~1.TXT).
[[nodiscard]] std::vector<BYTE> HardLinkImage();

// Under the root: Keep (16) with k.txt (17), and Gone (18), a deleted folder,
// with Mid (19) with orphan.txt (20). With no deleted files shown, Mid and
// orphan.txt have no parent that the volume holds.
[[nodiscard]] std::vector<BYTE> OrphanImage();

// Names that stress a search, with the record of each node in brackets. Under
// the root: Docs (16) with a file named Cafe with an acute e, then .txt (17), a
// Greek and a Cyrillic file in lowercase (18, 19) and report.txt (20, a hard
// link, also named "Copy of report.txt" in the root); the same three names in
// capitals with the extension .DOC (21 to 23); a file with a surrogate pair
// between a and b (24), axb.txt (25) and a file with an unpaired high surrogate
// between a and b (26); the deleted gone-e.txt, with an acute e (27); and Deep
// (28), a folder with Deeper (29), which holds deep.txt (30). The code of each
// name is in the definition.
[[nodiscard]] std::vector<BYTE> NamesImage();

// Clusters at the end of the volume that ReallocatedImage() gives to its files.
inline constexpr std::uint64_t reallocated_tail_clusters = 4;

// Under the root: live.bin (16) and the deleted old.bin (17) claim the same two
// clusters, so old.bin lost its data. The deleted free.bin (18) has one cluster
// of its own.
[[nodiscard]] std::vector<BYTE> ReallocatedImage();

// The record number of the tree node with the given index.
[[nodiscard]] constexpr std::uint64_t Record(std::size_t node) {
  return NTFS_BROWSER_NAMESPACE::Test::FakeImage::name_tree_first_record + node;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Search
