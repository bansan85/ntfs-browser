#include "public-api-damage-scenarios.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "record-edit.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using Image = std::vector<BYTE>;

// The records of the $MFT tree the inputs edit: a folder, a file with two hard
// links, a deleted file, and a second folder.
constexpr ULONGLONG docs_record = FakeImage::mft_tree_docs_idx;
constexpr ULONGLONG hard_link_record = FakeImage::mft_tree_hard_link_idx;
constexpr ULONGLONG deleted_file_record = FakeImage::mft_tree_deleted_file_idx;
constexpr ULONGLONG new_dir_record = FakeImage::mft_tree_reused_dir_idx;

// Where a file record header keeps its sequence number.
constexpr std::size_t sequence_field = 16;

// More units than any name of the tree has, and none.
constexpr BYTE name_units_past_value = 200;
constexpr BYTE name_units_none = 0;

// The sequence number of record idx.
WORD SequenceOf(const Image& image, ULONGLONG idx) {
  WORD sequence = 0;
  std::memcpy(&sequence,
              &image.at(RecordEdit::RecordOffset(idx) + sequence_field),
              sizeof(sequence));
  return sequence;
}

// Files record child under parent, whose sequence number is the one it has.
void FileUnder(Image& image, ULONGLONG child, ULONGLONG parent) {
  RecordEdit::SetFileNameParent(
      image, RecordEdit::Attribute(image, child, RecordEdit::type_file_name),
      parent, SequenceOf(image, parent));
}

// The $MFT tree where two folders are each other's parent: the path of either
// one has no root to reach, and the walk up closes on itself.
Image PathLoop() {
  Image image = FakeImage::BuildFakeNtfsImageWithMftTree();
  FileUnder(image, docs_record, new_dir_record);
  FileUnder(image, new_dir_record, docs_record);
  return image;
}

// The $MFT tree where two names cannot be decoded: one that claims more units
// than the attribute holds, and one that claims none.
Image DamagedNames() {
  Image image = FakeImage::BuildFakeNtfsImageWithMftTree();
  RecordEdit::SetFileNameLength(
      image,
      RecordEdit::Attribute(image, hard_link_record,
                            RecordEdit::type_file_name),
      name_units_past_value);
  RecordEdit::SetFileNameLength(
      image,
      RecordEdit::Attribute(image, deleted_file_record,
                            RecordEdit::type_file_name),
      name_units_none);
  return image;
}

}  // namespace

void PublicApiDamageScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown, as the other inputs of the
  // public API.
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded("mft_path_chain_loop", PathLoop, options);
  registry.AddRecorded("api_mft_list_path_loop", PathLoop, options,
                       RecordedStream::Probe::MftListApi);
  registry.AddRecorded("api_reader_damaged_names", DamagedNames, options,
                       RecordedStream::Probe::ReaderApi);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
