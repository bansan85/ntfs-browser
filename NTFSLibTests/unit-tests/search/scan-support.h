#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
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

// The names of rows, as UTF-8, in order.
[[nodiscard]] std::vector<std::string> Names(const std::vector<Row>& rows);

// The folder tree that most tests scan. Under the root: Docs (record 16) and
// root.txt (20). Docs holds a.txt (17), Sub (19) and the deleted gone.txt
// (21). Sub holds c.txt (18).
[[nodiscard]] std::vector<BYTE> TreeImage();

// The record number of the tree node with the given index.
[[nodiscard]] constexpr std::uint64_t Record(std::size_t node) {
  return NTFS_BROWSER_NAMESPACE::Test::FakeImage::name_tree_first_record + node;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Search
