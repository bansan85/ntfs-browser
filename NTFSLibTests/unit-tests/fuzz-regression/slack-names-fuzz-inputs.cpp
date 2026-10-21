#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "fuzz-input-writer.h"

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeSlackName;

namespace {

// One saved fuzz input: its file name, and the image it was recorded from.
struct Variant {
  std::string name;
  std::vector<BYTE> image;
};

// Bytes the fake record of the Docs directory keeps after its terminator,
// before the fixup array: from offset 8 after the end marker to byte 1017.
constexpr size_t record_slack_room = 490;

// Bytes a block of one live entry keeps after its end entry.
constexpr size_t index_slack_room = 800;

// Index block size of the fake volume: one cluster.
constexpr DWORD block_size = Tests::fake_cluster_size;

// Where the length byte of a $FILE_NAME sits, from the start of the structure.
// A slack that starts there has lost every field before the name.
constexpr size_t name_length_offset = 64;

// A name of five characters, the shortest a partial candidate may have.
constexpr const wchar_t* five_chars = L"dupes";

// A flag bit NTFS never sets in a $FILE_NAME.
constexpr DWORD unknown_flag = 0x40000000;

// A name left in the slack of the Docs record, as the bare $FILE_NAME
// structure.
FakeSlackName InRecord(std::wstring name) {
  return FakeSlackName{.name = std::move(name), .with_entry_header = false};
}

// The same, for a block of the root directory: filed under the root.
FakeSlackName InBlock(std::wstring name) {
  return FakeSlackName{.name = std::move(name),
                       .parent_record = Tests::index_slack_directory_record,
                       .parent_sequence = Tests::index_slack_directory_sequence,
                       .with_entry_header = true};
}

// Writes the structure of name at the end of slack.
void Append(std::vector<BYTE>& slack, const FakeSlackName& name) {
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(name);
  slack.insert(slack.end(), bytes.begin(), bytes.end());
}

// The freed tree with bytes written into the slack of the Docs directory.
std::vector<BYTE> RecordWith(std::span<const BYTE> slack) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutRecordSlack(image, Tests::mft_tree_docs_idx, 8,
                        slack.first(std::min(slack.size(), record_slack_room)));
  return image;
}

// The root directory with one block, whose slack is given.
std::vector<BYTE> BlockWith(std::vector<BYTE> slack) {
  slack.resize(std::min(slack.size(), index_slack_room));
  const std::vector<std::vector<BYTE>> blocks{Tests::BuildFakeIndexBlock(
      Tests::FakeIndexBlock{.live = {InBlock(L"live.txt")},
                            .slack = std::move(slack)},
      block_size)};
  return Tests::BuildFakeNtfsImageWithIndexBlocks(blocks);
}

// The tail of a structure, from its length byte on: what a slack holds when the
// end marker has overwritten the start of the structure.
std::vector<BYTE> TailOf(const FakeSlackName& name) {
  std::vector<BYTE> bytes = Tests::EncodeSlackName(name);
  bytes.erase(bytes.begin(),
              bytes.begin() + static_cast<std::ptrdiff_t>(name_length_offset));
  return bytes;
}

// Names whose characters are not a name: a surrogate pair, a high surrogate
// with no low one after it or at the end of the name, a lone low surrogate,
// and a path separator.
std::vector<BYTE> NameUnits() {
  std::vector<BYTE> slack;
  Append(slack, InRecord(L"a\xD83D\xDC1Cz"));
  Append(slack, InRecord(L"b\xD83Dx"));
  Append(slack, InRecord(L"c\xD83D"));
  Append(slack, InRecord(L"d\xDC1Cz"));
  Append(slack, InRecord(L"e/f"));
  return slack;
}

// Structures that fail a check each. The tails of two partial structures that
// the end marker cut come first: one with characters no plain text has, then,
// after the zero flags that let it get that far, one with too few characters.
// A structure with attribute flags NTFS never sets follows.
std::vector<BYTE> RejectedFields() {
  constexpr size_t zero_flags_bytes = 8;
  std::vector<BYTE> slack;
  const std::vector<BYTE> odd_tail =
      TailOf(InRecord(L"\x2603\x2603\x2603\x2603\x2603"));
  slack.insert(slack.end(), odd_tail.begin(), odd_tail.end());
  slack.insert(slack.end(), zero_flags_bytes, 0);
  const std::vector<BYTE> short_tail = TailOf(InRecord(L"abc"));
  slack.insert(slack.end(), short_tail.begin(), short_tail.end());
  FakeSlackName flags = InRecord(five_chars);
  flags.flags = unknown_flag;
  Append(slack, flags);
  return slack;
}

// The same name found again, then again with one field changed, so that each
// field of the comparison is the one that tells the copies apart.
template <typename Make>
std::vector<BYTE> Repeats(Make make, int changes) {
  std::vector<BYTE> slack;
  Append(slack, make(0));
  Append(slack, make(0));
  for (int change = 1; change <= changes; ++change) {
    Append(slack, make(change));
  }
  return slack;
}

// What each copy of the name changes in the record: 1 and 2 move the parent, 3
// the namespace, 4 the creation time.
FakeSlackName RecordRepeat(int change) {
  FakeSlackName name = InRecord(five_chars);
  switch (change) {
    case 1:
      name.parent_record = 41;
      break;
    case 2:
      name.parent_sequence = 2;
      break;
    case 3:
      name.name_space = 2;
      break;
    case 4:
      name.times.at(0) += 1;
      break;
    default:
      break;
  }
  return name;
}

// The same in a block, where the parent MUST stay the directory's own for the
// name to be kept: 1 moves the namespace, 2 to 5 each of the four times.
FakeSlackName BlockRepeat(int change) {
  FakeSlackName name = InBlock(five_chars);
  switch (change) {
    case 1:
      name.name_space = 2;
      break;
    case 2:
      name.times.at(0) += 1;
      break;
    case 3:
      name.times.at(1) += 1;
      break;
    case 4:
      name.times.at(2) += 1;
      break;
    case 5:
      name.times.at(3) += 1;
      break;
    default:
      break;
  }
  return name;
}

// The name Docs carries now, left in its own slack: once under another parent,
// which makes it a different name, and once under the parent it has, which
// makes it the live one.
std::vector<BYTE> LiveNames() {
  constexpr ULONGLONG other_parent = 40;
  constexpr ULONGLONG root_record = 5;
  std::vector<BYTE> slack;
  FakeSlackName elsewhere = InRecord(L"Docs");
  elsewhere.parent_record = other_parent;
  Append(slack, elsewhere);
  FakeSlackName same = InRecord(L"Docs");
  same.parent_record = root_record;
  same.parent_sequence = Tests::index_slack_directory_sequence;
  Append(slack, same);
  FakeSlackName unknown_parent = InRecord(L"Docs");
  unknown_parent.parent_record = 0xFFFFFFFF;
  Append(slack, unknown_parent);
  return slack;
}

// The inputs, one per way a leftover can repeat or mislead.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;
  variants.push_back({"record_slack_name_units", RecordWith(NameUnits())});
  variants.push_back(
      {"record_slack_rejected_fields", RecordWith(RejectedFields())});
  variants.push_back(
      {"record_slack_repeated_names", RecordWith(Repeats(RecordRepeat, 4))});
  variants.push_back({"record_slack_live_names", RecordWith(LiveNames())});
  variants.push_back(
      {"index_slack_repeated_names", BlockWith(Repeats(BlockRepeat, 5))});
  return variants;
}

}  // namespace

// Writes the saved fuzz inputs of names carved twice or carved from bytes that
// are not a name. The volume is opened recovering with deleted items shown,
// since only then does a scan carve the slack. Set NTFS_BROWSER_FUZZ_INPUT_DIR
// to the folder to write them in (the repository's NTFSLibTests/fuzz/data) and
// run this hidden test case by its tag.
TEST_CASE("write the slack names fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  const VolumeOptions recovering{.include_deleted = true,
                                 .recover_errors = true};
  for (Variant& variant : Variants()) {
    Tests::FuzzInputs::WriteRecordedInput(std::filesystem::path(dir) /
                                              variant.name,
                                          std::move(variant.image), recovering);
  }
}
