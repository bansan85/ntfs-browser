#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/fake-image/slack-inputs.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::FakeImage {

namespace {

// The bytes NTFS writes at the end of the attributes of a record.
constexpr std::array<BYTE, SlackInputs::terminator_bytes> terminator{
    0xFF, 0xFF, 0xFF, 0xFF, 0x82, 0x79, 0x47, 0x11};

// Index block size of the fake volume: one cluster.
constexpr DWORD block_size = fake_cluster_size;

// Characters of the longest name, and how many of its bytes the end of the
// room leaves in place: the name is cut short.
constexpr std::size_t longest_name_chars = 255;
constexpr std::size_t cut_name_bytes = 200;

// Offset of the first entry of a block, and of its size field inside the entry:
// a block of the damaged set claims an entry size of 0 there.
constexpr std::size_t first_entry_offset = 0x40;
constexpr std::size_t entry_size_offset = 8;

// Offset of the update sequence check word at the end of the first sector.
constexpr std::size_t first_sector_check_word = 510;

// Where the length byte of a $FILE_NAME sits, from the start of the structure.
// A slack that starts there has lost every field before the name.
constexpr std::size_t name_length_offset = 64;

// Zero bytes between the two partial structures of RejectedFields(): the flags
// that let the second one get as far as the length check.
constexpr std::size_t zero_flags_bytes = 8;

// A name of five characters, the shortest a partial candidate may have.
constexpr const wchar_t* five_chars = L"dupes";

// A flag bit NTFS never sets in a $FILE_NAME.
constexpr DWORD unknown_flag = 0x40000000;

// A parent record that does not exist, and one that is not the root.
constexpr ULONGLONG no_such_parent = 0xFFFFFFFF;
constexpr ULONGLONG other_parent = 40;

// Writes the structure of name at the end of slack.
void Append(std::vector<BYTE>& slack, const FakeSlackName& name) {
  const std::vector<BYTE> bytes = EncodeSlackName(name);
  slack.insert(slack.end(), bytes.begin(), bytes.end());
}

// The tail of a structure, from its length byte on: what a slack holds when the
// end marker has overwritten the start of the structure.
std::vector<BYTE> TailOf(const FakeSlackName& name) {
  std::vector<BYTE> bytes = EncodeSlackName(name);
  bytes.erase(bytes.begin(),
              bytes.begin() + static_cast<std::ptrdiff_t>(name_length_offset));
  return bytes;
}

// The same name twice, then once more for each change from 1 to changes.
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
  FakeSlackName name = SlackInputs::RecordBare(five_chars);
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

// What each copy changes in a block: 1 moves the namespace, 2 to 5 each of the
// four times.
FakeSlackName BlockRepeat(int change) {
  FakeSlackName name = SlackInputs::BlockEntry(five_chars);
  switch (change) {
    case 1:
      name.name_space = 2;
      break;
    case 2:
    case 3:
    case 4:
    case 5:
      name.times.at(static_cast<std::size_t>(change - 2)) += 1;
      break;
    default:
      break;
  }
  return name;
}

}  // namespace

FakeSlackName SlackInputs::RecordEntry(std::wstring name) {
  return FakeSlackName{.name = std::move(name), .with_entry_header = true};
}

FakeSlackName SlackInputs::RecordBare(std::wstring name) {
  return FakeSlackName{.name = std::move(name), .with_entry_header = false};
}

FakeSlackName SlackInputs::BlockEntry(std::wstring name) {
  return FakeSlackName{.name = std::move(name),
                       .parent_record = index_slack_directory_record,
                       .parent_sequence = index_slack_directory_sequence,
                       .with_entry_header = true};
}

std::vector<BYTE>
    SlackInputs::ImageWithRecordSlack(std::span<const BYTE> slack) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();
  PutRecordSlack(image, mft_tree_docs_idx, terminator_bytes,
                 slack.first(std::min(slack.size(), record_slack_room)));
  return image;
}

std::vector<BYTE> SlackInputs::BlockWithSlack(std::vector<BYTE> slack,
                                              ULONGLONG vcn) {
  slack.resize(std::min(slack.size(), index_slack_room));
  return BuildFakeIndexBlock(FakeIndexBlock{.live = {BlockEntry(L"live.txt")},
                                            .slack = std::move(slack),
                                            .vcn = vcn},
                             block_size);
}

std::vector<BYTE> SlackInputs::ImageWithBlockSlack(std::vector<BYTE> slack) {
  const std::vector<std::vector<BYTE>> blocks{BlockWithSlack(std::move(slack))};
  return BuildFakeNtfsImageWithIndexBlocks(blocks);
}

std::vector<BYTE> SlackInputs::ImageWithDamagedBlocks() {
  std::vector<BYTE> bad_magic =
      BlockWithSlack(EncodeSlackName(BlockEntry(L"a.txt")), 0);
  bad_magic.at(0) = 'X';
  std::vector<BYTE> zero_entry =
      BlockWithSlack(EncodeSlackName(BlockEntry(L"b.txt")), 1);
  zero_entry.at(first_entry_offset + entry_size_offset) = 0;
  zero_entry.at(first_entry_offset + entry_size_offset + 1) = 0;
  std::vector<BYTE> bad_usn =
      BlockWithSlack(EncodeSlackName(BlockEntry(L"c.txt")), 2);
  bad_usn.at(first_sector_check_word) ^= 0xFF;
  const std::vector<std::vector<BYTE>> blocks{
      bad_magic, zero_entry, bad_usn,
      BlockWithSlack(EncodeSlackName(BlockEntry(L"survivor.txt")), 3)};
  return BuildFakeNtfsImageWithIndexBlocks(blocks);
}

std::vector<BYTE> SlackInputs::DenseNames(const FakeSlackName& name,
                                          std::size_t room) {
  const std::vector<BYTE> bytes = EncodeSlackName(name);
  std::vector<BYTE> slack(room);
  for (std::size_t offset = 0; offset + bytes.size() <= slack.size();
       offset += 2) {
    std::ranges::copy(bytes,
                      slack.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return slack;
}

std::vector<BYTE> SlackInputs::NameRunsPastTheEnd(const FakeSlackName& keeper,
                                                  std::size_t room) {
  FakeSlackName longest = keeper;
  longest.name = std::wstring(longest_name_chars, L'n');
  const std::vector<BYTE> bytes = EncodeSlackName(longest);
  FakeSlackName fits = keeper;
  fits.name = L"keeper.txt";
  std::vector<BYTE> slack = EncodeSlackName(fits);
  slack.resize(room);
  const std::size_t start = slack.size() - cut_name_bytes;
  std::ranges::copy(std::span(bytes).first(cut_name_bytes),
                    slack.begin() + static_cast<std::ptrdiff_t>(start));
  return slack;
}

std::vector<BYTE> SlackInputs::StaleTerminators() {
  std::vector<BYTE> slack;
  Append(slack, RecordEntry(L"first-old.txt"));
  slack.insert(slack.end(), terminator.begin(), terminator.end());
  Append(slack, {.name = L"second-old.txt",
                 .parent_record = no_such_parent,
                 .with_entry_header = false});
  slack.insert(slack.end(), terminator.begin(), terminator.end());
  Append(slack, RecordEntry(L"third-old.txt"));
  slack.insert(slack.end(), terminator.begin(), terminator.end());
  slack.resize(std::min(slack.size(), record_slack_room));
  return slack;
}

std::vector<BYTE> SlackInputs::NameUnits() {
  std::vector<BYTE> slack;
  Append(slack, RecordBare(L"a\xD83D\xDC1Cz"));
  Append(slack, RecordBare(L"b\xD83Dx"));
  Append(slack, RecordBare(L"c\xD83D"));
  Append(slack, RecordBare(L"d\xDC1Cz"));
  Append(slack, RecordBare(L"e/f"));
  return slack;
}

std::vector<BYTE> SlackInputs::RejectedFields() {
  std::vector<BYTE> slack;
  const std::vector<BYTE> odd_tail =
      TailOf(RecordBare(L"\x2603\x2603\x2603\x2603\x2603"));
  slack.insert(slack.end(), odd_tail.begin(), odd_tail.end());
  slack.insert(slack.end(), zero_flags_bytes, 0);
  const std::vector<BYTE> short_tail = TailOf(RecordBare(L"abc"));
  slack.insert(slack.end(), short_tail.begin(), short_tail.end());
  FakeSlackName flags = RecordBare(five_chars);
  flags.flags = unknown_flag;
  Append(slack, flags);
  return slack;
}

std::vector<BYTE> SlackInputs::RepeatedRecordNames() {
  constexpr int changes = 4;
  return Repeats(RecordRepeat, changes);
}

std::vector<BYTE> SlackInputs::RepeatedBlockNames() {
  constexpr int changes = 5;
  return Repeats(BlockRepeat, changes);
}

std::vector<BYTE> SlackInputs::LiveNames() {
  constexpr ULONGLONG root_record = 5;
  std::vector<BYTE> slack;
  FakeSlackName elsewhere = RecordBare(L"Docs");
  elsewhere.parent_record = other_parent;
  Append(slack, elsewhere);
  FakeSlackName same = RecordBare(L"Docs");
  same.parent_record = root_record;
  same.parent_sequence = index_slack_directory_sequence;
  Append(slack, same);
  FakeSlackName unknown_parent = RecordBare(L"Docs");
  unknown_parent.parent_record = no_such_parent;
  Append(slack, unknown_parent);
  return slack;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::FakeImage
