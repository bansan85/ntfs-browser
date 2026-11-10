#include "public-api-scenarios.h"

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "utf/utf.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using FakeImage::FakeDataRun;
using FakeImage::FakeTreeLink;
using FakeImage::FakeTreeNode;
using Image = std::vector<BYTE>;

// The Win32 attribute bits the nodes of the tree carry.
constexpr DWORD attribute_read_only = 0x01;
constexpr DWORD attribute_hidden = 0x02;
constexpr DWORD attribute_archive = 0x20;

// The files of the crowded folder: enough names that its index no longer fits
// the record and spills into index blocks, which a walk reads one by one.
constexpr std::size_t crowded_files = 28;

// The VCN-aligned run the big file owns, and the bytes it declares: two
// clusters, the second one only partly used.
constexpr ULONGLONG big_file_lcn = 40;
constexpr ULONGLONG big_file_clusters = 2;
constexpr ULONGLONG big_file_bytes =
    FakeImage::fake_cluster_size + FakeImage::fake_cluster_size / 2;

// A name from UTF-16 units, as the tree stores it. It keeps an unpaired
// surrogate, which a wide literal could not on every host.
std::wstring Name(std::u16string_view units) {
  return NTFS_BROWSER_NAMESPACE::Utf::Utf16ToWide(units);
}

// A link of a node, filed under the node parent.
FakeTreeLink Link(std::wstring name, std::size_t parent) {
  return {.name = std::move(name), .parent = parent};
}

// The tree of names with every kind of node: folders three levels deep, files
// with attributes, times and sizes, a file with data clusters, a hard link, a
// name with its DOS alias, names with a character past the BMP and with an
// unpaired surrogate, deleted files and a deleted folder with a deleted file in
// it, and a folder with so many names that its index spills into blocks. The
// first eight nodes (records 16 to 23) are the ones the Reader reads by number.
Image MixedTree() {
  std::vector<FakeTreeNode> nodes;
  // 16
  nodes.push_back({.links = {Link(L"Docs", FakeImage::name_tree_root)},
                   .directory = true,
                   .attributes = attribute_archive});
  // 17
  nodes.push_back({.links = {Link(Name(u"Café.txt"), 0)},
                   .data_size = 10,
                   .index_size = 10,
                   .index_allocated_size = FakeImage::fake_cluster_size,
                   .attributes = attribute_read_only | attribute_hidden,
                   .times = {.created = FakeImage::std_info_create_time,
                             .modified = FakeImage::std_info_alter_time,
                             .mft_changed = FakeImage::std_info_mft_time,
                             .accessed = FakeImage::std_info_read_time}});
  // 18
  nodes.push_back({.links = {Link(L"big.bin", 0)},
                   .runs = {FakeDataRun{.lcn = big_file_lcn,
                                        .clusters = big_file_clusters}},
                   .stream_size = big_file_bytes,
                   .index_size = big_file_bytes,
                   .index_allocated_size =
                       big_file_clusters * FakeImage::fake_cluster_size});
  // 19
  nodes.push_back(
      {.links = {Link(L"report.txt", 0),
                 Link(L"Copy of report.txt", FakeImage::name_tree_root)},
       .data_size = 4});
  // 20
  nodes.push_back({.links = {Link(L"LongName.txt", FakeImage::name_tree_root),
                             {.name = L"LONGNA~1.TXT",
                              .parent = FakeImage::name_tree_root,
                              .name_space = 2}},
                   .data_size = 2});
  // 21
  nodes.push_back({.links = {Link(L"old.tmp", 0)},
                   .in_use = false,
                   .keep_entries = true,
                   .data_size = 3});
  // 22
  nodes.push_back({.links = {Link(L"Gone", FakeImage::name_tree_root)},
                   .directory = true,
                   .in_use = false,
                   .keep_entries = true});
  // 23
  nodes.push_back({.links = {Link(L"inside.txt", 6)},
                   .in_use = false,
                   .keep_entries = true,
                   .data_size = 1});
  // 24
  nodes.push_back({.links = {Link(L"Sub", 0)}, .directory = true});
  // 25
  nodes.push_back({.links = {Link(L"Deeper", 8)}, .directory = true});
  // 26
  nodes.push_back({.links = {Link(L"deep.txt", 9)}, .data_size = 6});
  // 27 and 28
  nodes.push_back(
      {.links = {Link(Name(u"a\U0001D11Eb.txt"), FakeImage::name_tree_root)},
       .data_size = 1});
  nodes.push_back({.links = {Link(Name(u"a\xD834"
                                       u"b.txt"),
                                  FakeImage::name_tree_root)},
                   .data_size = 1});
  // 29
  const std::size_t crowded = nodes.size();
  nodes.push_back({.links = {Link(L"Crowded", FakeImage::name_tree_root)},
                   .directory = true});
  for (std::size_t file = 0; file < crowded_files; ++file) {
    nodes.push_back(
        {.links = {Link(L"crowded-file-name-" + std::to_wstring(file) + L".dat",
                        crowded)},
         .data_size = 1});
  }
  return FakeImage::BuildFakeNtfsImageWithNameTree(nodes);
}

// The $MFT tree with a change journal that names a file the tree has and one it
// does not.
Image WithJournal() {
  Image image = FakeImage::BuildFakeNtfsImageWithMftTree();
  FakeImage::FakeUsnOptions options;
  options.records.push_back({.name = L"report.txt",
                             .file_record = FakeImage::mft_tree_report_idx,
                             .file_sequence = 3});
  options.records.push_back({.name = L"ghost.txt", .file_record = 90});
  FakeImage::PutUsnJournal(image, options);
  return image;
}

}  // namespace

void PublicApiScenarios::AddImage(
    ScenarioRegistry& registry, const std::string& stem,
    const std::function<std::vector<BYTE>()>& make, bool with_folder,
    Feature needs) {
  // Opened recovering with deleted items shown: the engines then read the
  // journal, salvage what is damaged and list the freed files.
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded("api_mft_list_" + stem, make, options,
                       RecordedStream::Probe::MftListApi, needs);
  registry.AddRecorded("api_directory_" + stem, make, options,
                       RecordedStream::Probe::DirectoryApi, needs);
  registry.AddRecorded("api_reader_" + stem, make, options,
                       RecordedStream::Probe::ReaderApi, needs);
  if (!with_folder) {
    return;
  }
  registry.AddRecorded("api_mft_list_folder_" + stem, make, options,
                       RecordedStream::Probe::MftListFolderApi, needs);
  registry.AddRecorded("api_directory_folder_" + stem, make, options,
                       RecordedStream::Probe::DirectoryFolderApi, needs);
}

void PublicApiScenarios::Register(ScenarioRegistry& registry) {
  AddImage(registry, "mixed_tree", MixedTree, true);
  AddImage(registry, "mft_tree", FakeImage::BuildFakeNtfsImageWithMftTree,
           true);
  AddImage(
      registry, "attribute_list",
      [] { return FakeImage::BuildFakeNtfsImageWithMftTreeAttributeList(); },
      false);
  AddImage(registry, "freed_tree",
           FakeImage::BuildFakeNtfsImageWithFreedFileTree, false);
  AddImage(registry, "named_stream",
           FakeImage::BuildFakeNtfsImageWithNamedDataStream, false);
  AddImage(registry, "journal", WithJournal, false);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
