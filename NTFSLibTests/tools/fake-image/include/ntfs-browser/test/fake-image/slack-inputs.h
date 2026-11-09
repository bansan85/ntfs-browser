#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::FakeImage {

// Hostile slack: the leftover bytes of a file record or of an index block that
// a scan carves names from. The saved fuzz inputs of the slack come from here.
class SlackInputs {
 public:
  // Bytes the fake record of the Docs directory keeps after its terminator,
  // before the fixup array: from offset 8 after the end marker to byte 1017.
  static constexpr std::size_t record_slack_room = 490;

  // Bytes a block of one live entry keeps after its end entry.
  static constexpr std::size_t index_slack_room = 800;

  // Bytes of the terminator NTFS writes at the end of the attributes, as it
  // sits in every real record.
  static constexpr std::size_t terminator_bytes = 8;

  // A name left in the slack of the Docs record, with its index entry header.
  [[nodiscard]] static FakeSlackName RecordEntry(std::wstring name);

  // A name left in the slack of the Docs record, as the bare $FILE_NAME.
  [[nodiscard]] static FakeSlackName RecordBare(std::wstring name);

  // A name left in an index block of the root directory: filed under the root,
  // with its entry header.
  [[nodiscard]] static FakeSlackName BlockEntry(std::wstring name);

  // The freed tree with slack written into the Docs directory, cut to
  // record_slack_room.
  [[nodiscard]] static std::vector<BYTE>
      ImageWithRecordSlack(std::span<const BYTE> slack);

  // One index block with one live entry and the given slack, cut to
  // index_slack_room.
  [[nodiscard]] static std::vector<BYTE> BlockWithSlack(std::vector<BYTE> slack,
                                                        ULONGLONG vcn = 0);

  // The root directory with that one block.
  [[nodiscard]] static std::vector<BYTE>
      ImageWithBlockSlack(std::vector<BYTE> slack);

  // Blocks that fail one check each (magic, entry size, update sequence), then
  // a block that is sound.
  [[nodiscard]] static std::vector<BYTE> ImageWithDamagedBlocks();

  // Candidates on top of each other: the structure of name written at every
  // even offset of room bytes, each copy cutting the one before it.
  [[nodiscard]] static std::vector<BYTE> DenseNames(const FakeSlackName& name,
                                                    std::size_t room);

  // A name that fits, then a name of 255 characters whose end the last byte of
  // the room cuts off. Both are filed like keeper.
  [[nodiscard]] static std::vector<BYTE>
      NameRunsPastTheEnd(const FakeSlackName& keeper, std::size_t room);

  // What an index root that shrank several times leaves: names separated by old
  // terminators, one of them with its parent reference cut by the marker.
  [[nodiscard]] static std::vector<BYTE> StaleTerminators();

  // Names whose characters are not a name: a surrogate pair, a high surrogate
  // with no low one after it or at the end of the name, a lone low surrogate,
  // and a path separator.
  [[nodiscard]] static std::vector<BYTE> NameUnits();

  // Structures that fail a check each: the tails of two partial structures
  // that the end marker cut, then a structure with attribute flags NTFS never
  // sets.
  [[nodiscard]] static std::vector<BYTE> RejectedFields();

  // The same name in a record, found again, then again with one field changed
  // (the parent, its sequence, the namespace, the creation time), so that each
  // field of the comparison tells the copies apart.
  [[nodiscard]] static std::vector<BYTE> RepeatedRecordNames();

  // The same in a block, where the parent MUST stay the directory's own for the
  // name to be kept: the namespace, then each of the four times changes.
  [[nodiscard]] static std::vector<BYTE> RepeatedBlockNames();

  // The name Docs carries now, left in its own slack: under another parent,
  // under the parent it has, and under a parent that cannot exist.
  [[nodiscard]] static std::vector<BYTE> LiveNames();
};

}  // namespace NTFS_BROWSER_NAMESPACE::Test::FakeImage
