#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/record/carved-name.h>

#include "catch2/catch_message.hpp"
#include "fake-ntfs-image.h"
#include "record/slack-carver.h"

namespace Record = NTFS_BROWSER_NAMESPACE::Record;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using Tests::FakeSlackName;

namespace {

// Size of a fake record, and where its attributes end.
constexpr size_t record_size = 1024;
constexpr size_t marker_at = 200;

// The eight bytes NTFS writes at the end of the attributes: the marker word,
// then four bytes that belong to the live record.
constexpr std::array<BYTE, 8> terminator{0xFF, 0xFF, 0xFF, 0xFF,
                                         0x82, 0x79, 0x47, 0x11};

// A record of zeros whose attributes end at marker_at.
std::vector<BYTE> EmptyRecord(size_t size = record_size) {
  std::vector<BYTE> record(size);
  std::ranges::copy(terminator, record.begin() + marker_at);
  return record;
}

// Copies bytes into record at offset.
void Put(std::vector<BYTE>& record, size_t offset,
         std::span<const BYTE> bytes) {
  std::ranges::copy(bytes,
                    record.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Writes a bare name at offset: the length byte, a Win32 namespace byte and the
// units, with no field of a $FILE_NAME before it.
void PutRawName(std::vector<BYTE>& record, size_t offset,
                const std::vector<WORD>& units) {
  record.at(offset) = static_cast<BYTE>(units.size());
  record.at(offset + 1) = 1;
  std::memcpy(&record.at(offset + 2), units.data(),
              units.size() * sizeof(WORD));
}

// Writes the terminator again, as NTFS does after the live attributes: it
// overwrites whatever leftover sat there.
void PutTerminator(std::vector<BYTE>& record) {
  Put(record, marker_at, terminator);
}

std::vector<Record::CarvedName> Carve(const std::vector<BYTE>& record,
                                      size_t marker = marker_at) {
  return Record::CarveFileNames(record, marker);
}

ULONGLONG Ticks(const FILETIME& time) {
  return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32U) |
         time.dwLowDateTime;
}

FakeSlackName Named(std::wstring name) {
  return FakeSlackName{.name = std::move(name), .with_entry_header = true};
}

// First offset after the terminator, where a live entry list would begin.
constexpr size_t after_terminator = marker_at + terminator.size();

}  // namespace

TEST_CASE("a complete name in the slack is carved", "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  const FakeSlackName slack = Named(L"report.txt");
  Put(record, after_terminator, Tests::EncodeSlackName(slack));

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 1);
  const Record::CarvedName& name = carved.front();
  CHECK(name.name == L"report.txt");
  CHECK(name.parent_known);
  CHECK(name.parent_record == 39);
  CHECK(name.parent_sequence == 1);
  CHECK(name.times_known);
  CHECK(Ticks(name.create_time) == slack.times.at(0));
  CHECK(Ticks(name.write_time) == slack.times.at(1));
  CHECK(Ticks(name.change_time) == slack.times.at(2));
  CHECK(Ticks(name.access_time) == slack.times.at(3));
  CHECK(name.name_space == 1);
  CHECK_FALSE(name.partial);
  CHECK(name.source == Record::CarveSource::RecordSlack);
}

TEST_CASE("several names are carved in the order they sit", "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  size_t offset = after_terminator;
  for (const std::wstring name : {L"first.txt", L"second.txt", L"third.txt"}) {
    const std::vector<BYTE> bytes = Tests::EncodeSlackName(Named(name));
    Put(record, offset, bytes);
    offset += bytes.size();
  }

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 3);
  CHECK(carved.at(0).name == L"first.txt");
  CHECK(carved.at(1).name == L"second.txt");
  CHECK(carved.at(2).name == L"third.txt");
}

TEST_CASE("a name on any even offset of the slack is found",
          "[record][slack]") {
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(Named(L"found.dat"));
  for (size_t offset = after_terminator; offset + bytes.size() <= record_size;
       offset += 2) {
    std::vector<BYTE> record = EmptyRecord();
    Put(record, offset, bytes);

    const auto carved = Carve(record);

    INFO("offset " << offset);
    REQUIRE(carved.size() == 1);
    CHECK(carved.front().name == L"found.dat");
  }
}

TEST_CASE("a name beyond the 2 bytes alignment is not looked for",
          "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  Put(record, after_terminator + 1, Tests::EncodeSlackName(Named(L"odd.dat")));

  CHECK(Carve(record).empty());
}

TEST_CASE("a name that starts inside the terminator has no parent",
          "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  // The structure begins on the four bytes after the marker word: its parent
  // reference is half the live record's.
  Put(record, marker_at + 4,
      Tests::EncodeSlackName({.name = L"t3.txt", .with_entry_header = false}));

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 1);
  const Record::CarvedName& name = carved.front();
  CHECK(name.name == L"t3.txt");
  CHECK_FALSE(name.parent_known);
  CHECK(name.parent_record == 0);
  CHECK(name.times_known);
  CHECK(Ticks(name.create_time) != 0);
  CHECK(name.partial);
}

TEST_CASE("a name whose start the live record overwrote is partial",
          "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  // The structure began before the marker: the live attributes overwrote its
  // parent reference and its times, and the terminator its last time.
  Put(record, marker_at - 16,
      Tests::EncodeSlackName({.name = L"t2.txt", .with_entry_header = false}));
  PutTerminator(record);

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 1);
  const Record::CarvedName& name = carved.front();
  CHECK(name.name == L"t2.txt");
  CHECK(name.partial);
  CHECK_FALSE(name.parent_known);
  CHECK_FALSE(name.times_known);
  CHECK(Ticks(name.create_time) == 0);
  CHECK(Ticks(name.access_time) == 0);
}

TEST_CASE("a partial name needs five characters", "[record][slack]") {
  struct Case {
    std::wstring name;
    bool carved;
  };

  for (const Case& test_case : {Case{L"a.c", false}, Case{L"ab.c", false},
                                Case{L"ab.cd", true}, Case{L"abc.def", true}}) {
    std::vector<BYTE> record = EmptyRecord();
    Put(record, marker_at - 16,
        Tests::EncodeSlackName(
            {.name = test_case.name, .with_entry_header = false}));
    PutTerminator(record);

    INFO(test_case.name.size());
    CHECK(Carve(record).empty() != test_case.carved);
  }
}

TEST_CASE("a name with no field left is carved from its characters only",
          "[record][slack]") {
  for (const size_t offset : {marker_at + 4, marker_at + 8, marker_at + 14}) {
    std::vector<BYTE> record = EmptyRecord();
    PutRawName(record, offset,
               {'l', 'o', 'n', 'e', 'l', 'y', '.', 't', 'x', 't'});

    const auto carved = Carve(record);

    INFO(offset);
    REQUIRE(carved.size() == 1);
    CHECK(carved.front().name == L"lonely.txt");
    CHECK(carved.front().partial);
    CHECK_FALSE(carved.front().parent_known);
    CHECK_FALSE(carved.front().times_known);
  }
}

TEST_CASE("a name with no field left needs plain characters",
          "[record][slack]") {
  // Private use characters: valid in a name, but no proof by themselves.
  std::vector<BYTE> odd_record = EmptyRecord();
  PutRawName(odd_record, marker_at + 8,
             {0xE001, 0xE002, 0xE003, 0xE004, 0xE005});
  CHECK(Carve(odd_record).empty());

  // Five characters of the Basic Multilingual Plane's CJK block are plain.
  std::vector<BYTE> cjk_record = EmptyRecord();
  PutRawName(cjk_record, marker_at + 8,
             {0x4E2D, 0x6587, 0x6587, 0x4EF6, 0x540D});
  CHECK(Carve(cjk_record).size() == 1);
}

TEST_CASE("a name with no field left and four characters is not carved",
          "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  PutRawName(record, marker_at + 8, {'a', 'b', 'c', 'd'});

  CHECK(Carve(record).empty());
}

TEST_CASE("an empty slack gives no name", "[record][slack]") {
  CHECK(Carve(EmptyRecord()).empty());
  CHECK(Carve(EmptyRecord(4096)).empty());
}

TEST_CASE("a slack of 0xFF bytes gives no name", "[record][slack]") {
  std::vector<BYTE> record(record_size, 0xFF);

  CHECK(Carve(record).empty());
}

TEST_CASE("random bytes give no name", "[record][slack]") {
  std::mt19937 engine(20'261'021);
  std::uniform_int_distribution<unsigned> byte(0, 255);
  for (int round = 0; round < 3000; ++round) {
    std::vector<BYTE> record(record_size);
    for (BYTE& value : record) {
      value = static_cast<BYTE>(byte(engine));
    }
    std::ranges::copy(terminator, record.begin() + marker_at);

    INFO("round " << round);
    CHECK(Carve(record).empty());
  }
}

TEST_CASE("random bytes with a plausible name header give no name",
          "[record][slack]") {
  std::mt19937 engine(7);
  std::uniform_int_distribution<unsigned> byte(0, 255);
  for (int round = 0; round < 500; ++round) {
    std::vector<BYTE> record(record_size);
    for (BYTE& value : record) {
      value = static_cast<BYTE>(byte(engine));
    }
    // Zero the parts a real structure holds as zero, and give a valid length
    // byte and namespace anywhere in the slack: still no proof.
    std::ranges::copy(terminator, record.begin() + marker_at);
    const size_t at = 300 + (static_cast<size_t>(byte(engine)) % 400);
    record.at(at) = 6;
    record.at(at + 1) = 1;

    INFO("round " << round);
    CHECK(Carve(record).empty());
  }
}

TEST_CASE("a copy of the same name is reported once", "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(Named(L"twin.txt"));
  Put(record, after_terminator, bytes);
  Put(record, after_terminator + bytes.size() + 24, bytes);

  CHECK(Carve(record).size() == 1);
}

TEST_CASE("the same name with other times is reported twice",
          "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  FakeSlackName first = Named(L"twin.txt");
  FakeSlackName second = first;
  second.times.at(0) += 0x1000;
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(first);
  Put(record, after_terminator, bytes);
  Put(record, after_terminator + bytes.size(), Tests::EncodeSlackName(second));

  CHECK(Carve(record).size() == 2);
}

TEST_CASE("each time must be plausible", "[record][slack]") {
  constexpr ULONGLONG year_1970 = 116'444'736'000'000'000ULL;
  constexpr ULONGLONG year_2200 = 189'216'000'000'000'000ULL;
  for (size_t which = 0; which < 4; ++which) {
    for (const ULONGLONG bad :
         {ULONGLONG{0}, year_1970, year_2200, ~ULONGLONG{0}}) {
      std::vector<BYTE> record = EmptyRecord();
      FakeSlackName slack = Named(L"clock.txt");
      slack.times.at(which) = bad;
      Put(record, after_terminator, Tests::EncodeSlackName(slack));

      INFO("time " << which << " = " << bad);
      CHECK(Carve(record).empty());
    }
  }
}

TEST_CASE("the edges of the plausible time range", "[record][slack]") {
  constexpr ULONGLONG year_1980 = 119'600'064'000'000'000ULL;
  constexpr ULONGLONG year_2100 = 157'469'184'000'000'000ULL;
  for (const auto& [time, accepted] :
       {std::pair{year_1980 - 1, false}, std::pair{year_1980, true},
        std::pair{year_2100 - 1, true}, std::pair{year_2100, false}}) {
    std::vector<BYTE> record = EmptyRecord();
    FakeSlackName slack = Named(L"edge.txt");
    slack.times.at(3) = time;
    Put(record, after_terminator, Tests::EncodeSlackName(slack));

    INFO(time);
    CHECK(Carve(record).empty() != accepted);
  }
}

TEST_CASE("attribute flags NTFS never sets reject the structure",
          "[record][slack]") {
  for (const DWORD flags :
       {DWORD{0x40000000}, DWORD{0x00010000}, DWORD{0x80000000}}) {
    std::vector<BYTE> record = EmptyRecord();
    FakeSlackName slack = Named(L"flags.txt");
    slack.flags = flags;
    Put(record, after_terminator, Tests::EncodeSlackName(slack));

    INFO(flags);
    CHECK(Carve(record).empty());
  }
}

TEST_CASE("a directory entry flag is accepted", "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  FakeSlackName slack = Named(L"folder");
  slack.flags = 0x10000000 | 0x20;
  Put(record, after_terminator, Tests::EncodeSlackName(slack));

  CHECK(Carve(record).size() == 1);
}

TEST_CASE("a namespace above 3 rejects the structure", "[record][slack]") {
  for (const BYTE name_space : {BYTE{4}, BYTE{0x80}, BYTE{0xFF}}) {
    std::vector<BYTE> record = EmptyRecord();
    FakeSlackName slack = Named(L"space.txt");
    slack.name_space = name_space;
    Put(record, after_terminator, Tests::EncodeSlackName(slack));

    INFO(+name_space);
    CHECK(Carve(record).empty());
  }
}

TEST_CASE("every namespace from 0 to 3 is accepted", "[record][slack]") {
  for (const BYTE name_space : {BYTE{0}, BYTE{1}, BYTE{2}, BYTE{3}}) {
    std::vector<BYTE> record = EmptyRecord();
    FakeSlackName slack = Named(L"space.txt");
    slack.name_space = name_space;
    Put(record, after_terminator, Tests::EncodeSlackName(slack));

    const auto carved = Carve(record);

    INFO(+name_space);
    REQUIRE(carved.size() == 1);
    CHECK(carved.front().name_space == name_space);
  }
}

TEST_CASE("a name with a forbidden character is rejected", "[record][slack]") {
  const std::vector<std::wstring> bad_names = {
      std::wstring(L"a\x01"
                   L"b.txt"),
      L"dir/file.txt",
      L"dir\\file.txt",
      std::wstring(L"del\x7F.txt"),
      std::wstring(L"c1\x85.txt"),
      std::wstring(L"nc\xFFFF.txt"),
      std::wstring(L"nc\xFFFE.txt"),
  };
  for (const std::wstring& name : bad_names) {
    std::vector<BYTE> record = EmptyRecord();
    Put(record, after_terminator, Tests::EncodeSlackName(Named(name)));

    INFO(name.size());
    CHECK(Carve(record).empty());
  }
}

TEST_CASE("a name with a lone surrogate is rejected", "[record][slack]") {
  // Built from raw units: wchar_t holds a whole code point off Windows.
  for (const std::array<WORD, 2> units :
       {std::array<WORD, 2>{0xD800, 0x0041},
        std::array<WORD, 2>{0xDC00, 0x0041},
        std::array<WORD, 2>{0x0041, 0xD800}}) {
    std::vector<BYTE> record = EmptyRecord();
    std::vector<BYTE> bytes = Tests::EncodeSlackName(Named(L"xx"));
    // Entry header 16, then the name at 0x42: the two units.
    std::memcpy(&bytes.at(16 + 0x42), units.data(), sizeof(units));
    Put(record, after_terminator, bytes);

    CHECK(Carve(record).empty());
  }
}

TEST_CASE("a surrogate pair in a name is accepted", "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  std::vector<BYTE> bytes = Tests::EncodeSlackName(Named(L"xx"));
  const std::array<WORD, 2> pair{0xD83D, 0xDE00};
  std::memcpy(&bytes.at(16 + 0x42), pair.data(), sizeof(pair));
  Put(record, after_terminator, bytes);

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 1);
  CHECK_FALSE(carved.front().name.empty());
}

TEST_CASE("a name of 255 characters is carved", "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord(4096);
  const std::wstring name(255, L'n');
  Put(record, after_terminator, Tests::EncodeSlackName(Named(name)));

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 1);
  CHECK(carved.front().name == name);
}

TEST_CASE("a name that ends on the last byte of the record is carved",
          "[record][slack]") {
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(Named(L"edge.txt"));
  // The padding to 8 bytes is not part of the name.
  const size_t name_end = 16 + 0x42 + (8 * 2);
  std::vector<BYTE> record = EmptyRecord(marker_at + 8 + name_end);
  Put(record, after_terminator, std::span(bytes).first(name_end));

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 1);
  CHECK(carved.front().name == L"edge.txt");
}

TEST_CASE("a name that runs one byte past the record is not carved",
          "[record][slack]") {
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(Named(L"edge.txt"));
  const size_t name_end = 16 + 0x42 + (8 * 2);
  std::vector<BYTE> record = EmptyRecord(marker_at + 8 + name_end - 1);
  Put(record, after_terminator, std::span(bytes).first(name_end - 1));

  CHECK(Carve(record).empty());
}

TEST_CASE("a length byte of 255 near the record end reads nothing past it",
          "[record][slack]") {
  for (size_t tail = 2; tail < 40; ++tail) {
    std::vector<BYTE> record = EmptyRecord(marker_at + 8 + tail);
    record.at(marker_at + 8) = 255;
    record.at(marker_at + 9) = 1;

    INFO(tail);
    CHECK(Carve(record).empty());
  }
}

TEST_CASE("a parent reference to record 0 is unknown", "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  FakeSlackName slack = Named(L"orphan.txt");
  slack.parent_record = 0;
  slack.parent_sequence = 0;
  Put(record, after_terminator, Tests::EncodeSlackName(slack));

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 1);
  CHECK_FALSE(carved.front().parent_known);
  CHECK(carved.front().times_known);
  CHECK(carved.front().partial);
}

TEST_CASE("a parent sequence number is kept", "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  FakeSlackName slack = Named(L"seq.txt");
  slack.parent_record = 0x00ABCDEFULL;
  slack.parent_sequence = 0x7FFE;
  Put(record, after_terminator, Tests::EncodeSlackName(slack));

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 1);
  CHECK(carved.front().parent_record == 0x00ABCDEFULL);
  CHECK(carved.front().parent_sequence == 0x7FFE);
}

TEST_CASE("a marker at or past the end of the record gives nothing",
          "[record][slack]") {
  const std::vector<BYTE> record = EmptyRecord();
  for (const size_t marker :
       {record.size() - 4, record.size() - 3, record.size() - 1, record.size(),
        record.size() + 1, ~size_t{0} / 2, ~size_t{0}}) {
    INFO(marker);
    CHECK(Carve(record, marker).empty());
  }
}

TEST_CASE("tiny records give nothing", "[record][slack]") {
  for (size_t size = 0; size < 80; ++size) {
    const std::vector<BYTE> record(size, 0xFF);
    for (size_t marker = 0; marker < size + 2; ++marker) {
      CHECK(Carve(record, marker).empty());
    }
  }
}

TEST_CASE("a structure ending where the used size ends is carved",
          "[record][slack]") {
  // Leftover bytes sit past the used size of the record, up to its allocated
  // size: only the bytes after the terminator count.
  std::vector<BYTE> record = EmptyRecord();
  Put(record, record_size - 128, Tests::EncodeSlackName(Named(L"late.txt")));

  const auto carved = Carve(record);

  REQUIRE(carved.size() == 1);
  CHECK(carved.front().name == L"late.txt");
}

TEST_CASE("the scan stops on dense overlapping candidates", "[record][slack]") {
  const std::vector<BYTE> bytes = Tests::EncodeSlackName(Named(L"dense.txt"));
  for (const size_t stride : {size_t{2}, size_t{4}, size_t{8}, size_t{10}}) {
    std::vector<BYTE> record = EmptyRecord(4096);
    for (size_t offset = after_terminator;
         offset + bytes.size() <= record.size(); offset += stride) {
      Put(record, offset, bytes);
    }

    const auto carved = Carve(record);

    INFO(stride);
    CHECK_FALSE(carved.empty());
    CHECK(carved.size() <= record.size() / 4);
  }
}

TEST_CASE("a slack of a repeating name header terminates", "[record][slack]") {
  // Length 1 and namespace 0 over and over: the shortest candidate everywhere.
  std::vector<BYTE> record = EmptyRecord(4096);
  for (size_t offset = after_terminator; offset + 1 < record.size();
       offset += 2) {
    record.at(offset) = 1;
    record.at(offset + 1) = 0;
  }
  // Characters of 0x0001 are controls: nothing is carved, in finite time.
  CHECK(Carve(record).empty());

  for (size_t offset = after_terminator; offset + 3 < record.size();
       offset += 4) {
    record.at(offset) = 1;
    record.at(offset + 1) = 0;
    record.at(offset + 2) = 'a';
    record.at(offset + 3) = 0;
  }
  // Single-character partial names are too short to count.
  CHECK(Carve(record).empty());
}

TEST_CASE("the carver does not change its input", "[record][slack]") {
  std::vector<BYTE> record = EmptyRecord();
  Put(record, after_terminator, Tests::EncodeSlackName(Named(L"still.txt")));
  const std::vector<BYTE> before = record;

  static_cast<void>(Carve(record));

  CHECK(record == before);
}

TEST_CASE("a parent record number that cannot exist is unknown",
          "[record][slack]") {
  for (const ULONGLONG parent : {0xFFFFFFFFULL, 0x1FFFFFFFFULL}) {
    std::vector<BYTE> record = EmptyRecord();
    FakeSlackName slack = Named(L"marker.txt");
    slack.parent_record = parent;
    Put(record, after_terminator, Tests::EncodeSlackName(slack));

    const auto carved = Carve(record);

    INFO(parent);
    REQUIRE(carved.size() == 1);
    CHECK_FALSE(carved.front().parent_known);
    CHECK(carved.front().partial);
  }
}

TEST_CASE("a zero slack of any length gives no name", "[record][slack]") {
  for (size_t size = after_terminator; size < after_terminator + 80; ++size) {
    INFO(size);
    CHECK(Carve(EmptyRecord(size)).empty());
  }
}

TEST_CASE("a name found on any offset of a word and any tail length",
          "[record][slack]") {
  constexpr size_t name_bytes = 2 + (5 * sizeof(WORD));
  for (size_t lane = 0; lane < 8; ++lane) {
    for (size_t tail = 0; tail < 10; ++tail) {
      const size_t offset = after_terminator + (lane * 2);
      std::vector<BYTE> record = EmptyRecord(offset + name_bytes + tail);
      PutRawName(record, offset, {'a', 'b', 'c', 'd', 'e'});

      const auto carved = Carve(record);

      INFO("lane " << lane << " tail " << tail);
      REQUIRE(carved.size() == 1);
      CHECK(carved.front().name == L"abcde");
      CHECK(carved.front().partial);
    }
  }
}

TEST_CASE("a name header with the last possible start is found",
          "[record][slack]") {
  constexpr size_t name_bytes = 2 + (5 * sizeof(WORD));
  for (size_t extra = 0; extra < 16; extra += 2) {
    std::vector<BYTE> record =
        EmptyRecord(after_terminator + extra + name_bytes);
    PutRawName(record, record.size() - name_bytes, {'l', 'a', 's', 't', '!'});

    const auto carved = Carve(record);

    INFO(extra);
    REQUIRE(carved.size() == 1);
    CHECK(carved.front().name == L"last!");
  }
}

TEST_CASE("a slack of words that no name header fits gives no name",
          "[record][slack]") {
  // Length zero, namespace 4 and above, then every mix of them: no word is a
  // header, whatever the bytes around it hold.
  constexpr std::array<std::array<BYTE, 2>, 6> words{
      {{0, 1}, {5, 4}, {5, 0xFF}, {0, 0}, {9, 0x80}, {0xFF, 7}}};
  for (size_t first = 0; first < words.size(); ++first) {
    std::vector<BYTE> record = EmptyRecord(4096);
    size_t pick = first;
    for (size_t offset = after_terminator; offset + 1 < record.size();
         offset += 2) {
      record.at(offset) = words.at(pick).at(0);
      record.at(offset + 1) = words.at(pick).at(1);
      pick = (pick + 1) % words.size();
    }

    INFO(first);
    CHECK(Carve(record).empty());
  }
}
