// Tests for classic NTFS attribute-level compression
// (FILE_ATTRIBUTE_COMPRESSED plus a non-zero
// Attr::HeaderNonResident::comp_unit_size, LZNT1-encoded compression units):
// the LZNT1 chunk decoder on its own (src/lznt1/decompress.h), then the full
// AttrNonResident<S>::ReadData() path over synthetic volumes, one case per
// per-unit encoding a real compressed attribute can use.
//
// Before this support existed, FileRecord<S>::ParseAttrs()
// (src/file-record.cpp) rejected any record whose STANDARD_INFORMATION/
// FILE_NAME flags marked it FILE_ATTRIBUTE_COMPRESSED ("Compressed and
// Encrypted file not supported yet !"), so every compressed file and
// directory on a real volume was silently lost - and even with that lifted,
// AttrNonResident never read comp_unit_size, so compressed clusters came
// back as raw, undecoded bytes.

#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "lznt1/decompress.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// Sentinel fill so untouched bytes are distinguishable from valid (zero) data.
constexpr BYTE kSentinelByte = 0xCC;

// An opened synthetic volume plus the parsed root FileRecord, kept together
// because the record's attributes reference both (the volume for reads, the
// record buffer for their own header bytes).
template <Strategy S>
struct ParsedRoot
{
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<FileRecord<S>> record;
};

template <Strategy S>
ParsedRoot<S> ParseRoot(std::vector<BYTE> image)
{
  ParsedRoot<S> parsed;
  parsed.volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
  REQUIRE(parsed.volume->IsVolumeOK());

  parsed.record = std::make_unique<FileRecord<S>>(*parsed.volume);
  REQUIRE(parsed.record->ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  return parsed;
}

// Reads "size" bytes at "offset" of the root record's single $DATA
// attribute, truncated to what ReadData() actually produced; empty on failure.
template <Strategy S>
std::optional<std::vector<BYTE>> ReadRootData(const FileRecord<S>& record,
                                              ULONGLONG offset, size_t size)
{
  const auto& dataAttrs = record.getAttr(AttrType::DATA);
  REQUIRE(dataAttrs.size() == 1);

  std::vector<BYTE> buffer(size, kSentinelByte);
  const std::optional<ULONGLONG> read = dataAttrs[0]->ReadData(offset, buffer);
  if (!read)
  {
    return {};
  }

  buffer.resize(static_cast<size_t>(*read));
  return buffer;
}

// Captures everything the library logs while body runs - the only
// externally visible signal that a cached decompression was reused. The
// library logger is already pinned to a trace-level capturing sink; see
// test-log-sink.h.
std::string CaptureTrace(const std::function<void()>& body)
{
  (void)NtfsBrowserTests::TakeCapturedLog();
  body();
  return NtfsBrowserTests::TakeCapturedLog();
}

}  // namespace

////////////////////////////////////////////////////////////////////////////
// The LZNT1 chunk decoder on its own
////////////////////////////////////////////////////////////////////////////

TEST_CASE("LZNT1 decompresses the [MS-XCA] section 3.3 worked example",
          "[lznt1][compression]")
{
  std::vector<BYTE> out(NtfsBrowser::Lznt1::kChunkSize, kSentinelByte);

  const size_t produced = NtfsBrowser::Lznt1::Decompress(
      NtfsBrowserTests::kXcaLznt1ExampleCompressed, out);

  REQUIRE(produced == NtfsBrowserTests::kXcaLznt1ExampleDecompressedSize);
  CHECK(std::memcmp(out.data(), NtfsBrowserTests::kXcaLznt1ExampleDecompressed,
                    produced) == 0);
}

TEST_CASE("LZNT1 passes an uncompressed chunk through unchanged",
          "[lznt1][compression]")
{
  const std::vector<BYTE> payload =
      NtfsBrowserTests::CompressionFixturePattern(1234);
  const std::vector<BYTE> chunk =
      NtfsBrowserTests::MakeUncompressedLznt1Chunk(payload);
  REQUIRE(chunk.size() == payload.size() + 2);

  std::vector<BYTE> out(NtfsBrowser::Lznt1::kChunkSize, kSentinelByte);
  const size_t produced = NtfsBrowser::Lznt1::Decompress(chunk, out);

  REQUIRE(produced == payload.size());
  CHECK(std::memcmp(out.data(), payload.data(), payload.size()) == 0);
}

TEST_CASE("LZNT1 concatenates chunks and stops at End_of_buffer",
          "[lznt1][compression]")
{
  const std::vector<BYTE> first =
      NtfsBrowserTests::CompressionFixturePattern(100);
  const std::vector<BYTE> second =
      NtfsBrowserTests::CompressionFixturePattern(50);

  std::vector<BYTE> src = NtfsBrowserTests::MakeUncompressedLznt1Chunk(first);
  const std::vector<BYTE> secondChunk =
      NtfsBrowserTests::MakeUncompressedLznt1Chunk(second);
  src.insert(src.end(), secondChunk.begin(), secondChunk.end());
  // End_of_buffer, followed by bytes that must never be looked at.
  src.insert(src.end(), {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF});

  std::vector<BYTE> out(NtfsBrowser::Lznt1::kChunkSize, kSentinelByte);
  const size_t produced = NtfsBrowser::Lznt1::Decompress(src, out);

  REQUIRE(produced == first.size() + second.size());
  CHECK(std::memcmp(out.data(), first.data(), first.size()) == 0);
  CHECK(std::memcmp(out.data() + first.size(), second.data(), second.size()) ==
        0);
}

TEST_CASE("LZNT1 rejects malformed input instead of reading out of bounds",
          "[lznt1][compression]")
{
  std::vector<BYTE> out(NtfsBrowser::Lznt1::kChunkSize, kSentinelByte);

  SECTION("chunk header with a wrong signature")
  {
    // Bits 14-12 must always be 3; here they are 0.
    const std::vector<BYTE> src{0x02, 0x80, 0x01, 0x00, 0x00};
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, out),
                    std::runtime_error);
  }

  SECTION("chunk declaring more data than the buffer holds")
  {
    // Declared chunk size 0x0FFF + 3 == 4098 bytes, but only 4 are here.
    const std::vector<BYTE> src{0xFF, 0xBF, 0x00, 0x00};
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, out),
                    std::runtime_error);
  }

  SECTION("compressed word truncated by the chunk's own declared size")
  {
    // Declared size 4 bytes = 2 payload bytes: a compressed-word flag, then
    // only 1 of its 2 bytes.
    const std::vector<BYTE> src{0x01, 0xb0, 0x01, 0x00};
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, out),
                    std::runtime_error);
  }

  SECTION("back-reference pointing before the start of the chunk")
  {
    // Same bytes BuildFakeNtfsImageWithCorruptCompressedUnit() writes:
    // displacement 1, nothing decompressed yet.
    const std::vector<BYTE> src{0x02, 0xb0, 0x01, 0x00, 0x00};
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, out),
                    std::runtime_error);
  }

  SECTION("chunk decompressing to more than 4096 bytes")
  {
    // Decodes to 4099 bytes: 1 over the 4096 per-chunk limit ([MS-XCA] 2.5.3).
    const std::vector<BYTE> src{0x03, 0xb0, 0x02, 0x41, 0xff, 0x0f};
    // Roomy dest (2 chunks) so the per-chunk 4096 limit rejects this, not
    // the buffer bound.
    std::vector<BYTE> roomy(2 * NtfsBrowser::Lznt1::kChunkSize, kSentinelByte);
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, roomy),
                    std::runtime_error);
  }

  SECTION("output overrunning the destination buffer")
  {
    // Well-formed 100-byte chunk into a 10-byte dest: a real unit never
    // exceeds its own size, so this is malformed.
    const std::vector<BYTE> src = NtfsBrowserTests::MakeUncompressedLznt1Chunk(
        NtfsBrowserTests::CompressionFixturePattern(100));
    std::vector<BYTE> tiny(10, kSentinelByte);
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, tiny),
                    std::runtime_error);
  }
}

////////////////////////////////////////////////////////////////////////////
// End-to-end: AttrNonResident<S>::ReadData() over compressed attributes
////////////////////////////////////////////////////////////////////////////

namespace
{

// I/O matrix row "Compressed unit": one real run of [MS-XCA] section 3.3
// LZNT1 bytes, then a sparse run padding out the full cluster count.
template <Strategy S>
void CheckCompressedFileReadsBackDecompressed()
{
  ParsedRoot<S> root =
      ParseRoot<S>(NtfsBrowserTests::BuildFakeNtfsImageWithCompressedFile());

  // Acceptance criterion: a compressed record is no longer skipped.
  REQUIRE(root.record->ParseAttrs());
  CHECK(root.record->IsCompressed());
  CHECK_FALSE(root.record->IsEncrypted());
  // real_size, not GetFileSize(): these minimal fixtures carry no $FILE_NAME.
  REQUIRE(root.record->getAttr(AttrType::DATA).size() == 1);
  CHECK(root.record->getAttr(AttrType::DATA)[0]->GetDataSize() ==
        NtfsBrowserTests::kXcaLznt1ExampleDecompressedSize);

  const std::optional<std::vector<BYTE>> data = ReadRootData<S>(
      *root.record, 0, NtfsBrowserTests::kXcaLznt1ExampleDecompressedSize);
  REQUIRE(data.has_value());
  REQUIRE(data->size() == NtfsBrowserTests::kXcaLznt1ExampleDecompressedSize);
  CHECK(std::memcmp(data->data(),
                    NtfsBrowserTests::kXcaLznt1ExampleDecompressed,
                    data->size()) == 0);
}

// I/O matrix row "Stored (incompressible) unit": real runs fill the whole
// unit with no sparse pad, so the bytes must pass through undecompressed.
template <Strategy S>
void CheckStoredCompressionUnitReadsBackVerbatim()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithStoredCompressionUnit());
  REQUIRE(root.record->ParseAttrs());

  const std::vector<BYTE> expected =
      NtfsBrowserTests::CompressionFixturePattern(
          NtfsBrowserTests::kCompressionUnitSize);

  const std::optional<std::vector<BYTE>> data =
      ReadRootData<S>(*root.record, 0, expected.size());
  REQUIRE(data.has_value());
  REQUIRE(data->size() == expected.size());
  CHECK(std::memcmp(data->data(), expected.data(), expected.size()) == 0);
}

// I/O matrix row "Fully sparse unit": no real cluster at all, so it must
// read back zero-filled, same as the pre-existing uncompressed sparse path.
template <Strategy S>
void CheckSparseCompressionUnitReadsBackZeroed()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSparseCompressionUnit());
  REQUIRE(root.record->ParseAttrs());

  const std::optional<std::vector<BYTE>> data =
      ReadRootData<S>(*root.record, 0, NtfsBrowserTests::kCompressionUnitSize);
  REQUIRE(data.has_value());
  REQUIRE(data->size() == NtfsBrowserTests::kCompressionUnitSize);

  size_t nonZero = 0;
  for (const BYTE b : *data)
  {
    if (b != 0)
    {
      nonZero++;
    }
  }
  CHECK(nonZero == 0);
}

// A unit whose compressed bytes span TWO non-contiguous data runs, which the
// read path must stitch together before decompressing.
template <Strategy S>
void CheckFragmentedCompressedFileReadsBackDecompressed()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFragmentedCompressedFile());
  REQUIRE(root.record->ParseAttrs());

  const std::vector<BYTE> expected =
      NtfsBrowserTests::CompressionFixturePattern(
          NtfsBrowserTests::kFragmentedCompressedPayloadSize);

  const std::optional<std::vector<BYTE>> data =
      ReadRootData<S>(*root.record, 0, expected.size());
  REQUIRE(data.has_value());
  REQUIRE(data->size() == expected.size());
  CHECK(std::memcmp(data->data(), expected.data(), expected.size()) == 0);
}

// I/O matrix row "Trailing partial unit at EOF": real_size is not a multiple
// of the compression unit size, so the trailing unit's real extent comes
// from a PARTIAL run. Reads must return exactly real_size bytes.
template <Strategy S>
void CheckTrailingPartialCompressionUnit()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithTrailingPartialCompressionUnit());
  REQUIRE(root.record->ParseAttrs());

  const std::vector<BYTE> head = NtfsBrowserTests::CompressionFixturePattern(
      NtfsBrowserTests::kTrailingPartialUnitStoredSize);
  const std::vector<BYTE> tail = NtfsBrowserTests::CompressionFixturePattern(
      NtfsBrowserTests::kTrailingPartialUnitTailSize);
  const size_t total = head.size() + tail.size();

  REQUIRE(root.record->getAttr(AttrType::DATA).size() == 1);
  CHECK(root.record->getAttr(AttrType::DATA)[0]->GetDataSize() == total);

  // One read spanning both units: stored, then compressed trailing.
  const std::optional<std::vector<BYTE>> whole =
      ReadRootData<S>(*root.record, 0, total);
  REQUIRE(whole.has_value());
  REQUIRE(whole->size() == total);
  CHECK(std::memcmp(whole->data(), head.data(), head.size()) == 0);
  CHECK(std::memcmp(whole->data() + head.size(), tail.data(), tail.size()) ==
        0);

  // Oversized read past EOF must truncate to the real count, not invent
  // bytes from sparse padding.
  const std::optional<std::vector<BYTE>> beyond = ReadRootData<S>(
      *root.record, head.size(), NtfsBrowserTests::kCompressionUnitSize);
  REQUIRE(beyond.has_value());
  REQUIRE(beyond->size() == tail.size());
  CHECK(std::memcmp(beyond->data(), tail.data(), tail.size()) == 0);
}

// I/O matrix row "Corrupt/truncated LZNT1 chunk": ReadData() must fail
// through its std::optional error channel, not crash or return garbage.
template <Strategy S>
void CheckCorruptCompressedUnitIsRejected()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCorruptCompressedUnit());
  REQUIRE(root.record->ParseAttrs());

  std::optional<std::vector<BYTE>> data;
  const std::string trace = CaptureTrace(
      [&]
      {
        data =
            ReadRootData<S>(*root.record, 0,
                            NtfsBrowserTests::kXcaLznt1ExampleDecompressedSize);
      });

  CHECK_FALSE(data.has_value());
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "LZNT1: back-reference before start of chunk."));
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "Cannot decompress compression unit 0"));
}

// A unit no data run maps at all must make ReadData() fail, not hand back
// fabricated zeroes as if it were a hole.
template <Strategy S>
void CheckUnmappedCompressionUnitIsRejected()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUnmappedCompressionUnit());
  REQUIRE(root.record->ParseAttrs());

  const std::vector<BYTE> expected =
      NtfsBrowserTests::CompressionFixturePattern(
          NtfsBrowserTests::kCompressionUnitSize);

  // Unit 0 is well-formed and must still read back.
  const std::optional<std::vector<BYTE>> mapped =
      ReadRootData<S>(*root.record, 0, expected.size());
  REQUIRE(mapped.has_value());
  REQUIRE(mapped->size() == expected.size());
  CHECK(std::memcmp(mapped->data(), expected.data(), expected.size()) == 0);

  // Reading into unit 1 must not be answered with zeroes.
  std::optional<std::vector<BYTE>> unmapped;
  const std::string trace = CaptureTrace(
      [&]
      {
        unmapped = ReadRootData<S>(*root.record, 0,
                                   2 * NtfsBrowserTests::kCompressionUnitSize);
      });

  CHECK_FALSE(unmapped.has_value());
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring("is not fully mapped"));
}

// A real run after a hole inside one unit is not an encoding this code
// understands: a compressed unit's sparse padding is always its tail.
template <Strategy S>
void CheckRealClustersAfterHoleIsRejected()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithRealClustersAfterHole());
  REQUIRE(root.record->ParseAttrs());

  std::optional<std::vector<BYTE>> data;
  const std::string trace = CaptureTrace(
      [&]
      {
        data = ReadRootData<S>(*root.record, 0,
                               NtfsBrowserTests::kCompressionUnitSize);
      });

  CHECK_FALSE(data.has_value());
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "has real clusters after a hole"));
}

// An interior unit whose LZNT1 stream ends early must not be zero-padded
// and returned as valid data (unlike a legitimate trailing partial unit).
template <Strategy S>
void CheckShortDecompressedInteriorUnitIsRejected()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithShortDecompressedUnit());
  REQUIRE(root.record->ParseAttrs());

  std::optional<std::vector<BYTE>> data;
  const std::string trace = CaptureTrace(
      [&]
      {
        data = ReadRootData<S>(*root.record, 0,
                               NtfsBrowserTests::kCompressionUnitSize);
      });

  CHECK_FALSE(data.has_value());
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "decompressed to 100 bytes, expected at least 4096"));
}

}  // namespace

TEST_CASE("A compressed file's data reads back decompressed",
          "[attr-non-resident][compression]")
{
  CheckCompressedFileReadsBackDecompressed<Strategy::NO_CACHE>();
}

TEST_CASE("A compressed file's data reads back decompressed (FULL_CACHE)",
          "[attr-non-resident][compression]")
{
  CheckCompressedFileReadsBackDecompressed<Strategy::FULL_CACHE>();
}

TEST_CASE("A stored (incompressible) compression unit reads back verbatim",
          "[attr-non-resident][compression]")
{
  CheckStoredCompressionUnitReadsBackVerbatim<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A stored (incompressible) compression unit reads back verbatim "
    "(FULL_CACHE)",
    "[attr-non-resident][compression]")
{
  CheckStoredCompressionUnitReadsBackVerbatim<Strategy::FULL_CACHE>();
}

TEST_CASE("A fully sparse compression unit reads back zero-filled",
          "[attr-non-resident][compression]")
{
  CheckSparseCompressionUnitReadsBackZeroed<Strategy::NO_CACHE>();
}

TEST_CASE("A fully sparse compression unit reads back zero-filled (FULL_CACHE)",
          "[attr-non-resident][compression]")
{
  CheckSparseCompressionUnitReadsBackZeroed<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "A compression unit whose compressed bytes span several data runs is "
    "stitched back together before decompressing",
    "[attr-non-resident][compression]")
{
  CheckFragmentedCompressedFileReadsBackDecompressed<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A compression unit whose compressed bytes span several data runs is "
    "stitched back together before decompressing (FULL_CACHE)",
    "[attr-non-resident][compression]")
{
  CheckFragmentedCompressedFileReadsBackDecompressed<Strategy::FULL_CACHE>();
}

TEST_CASE("A trailing partial compression unit returns exactly real_size bytes",
          "[attr-non-resident][compression]")
{
  CheckTrailingPartialCompressionUnit<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A trailing partial compression unit returns exactly real_size bytes "
    "(FULL_CACHE)",
    "[attr-non-resident][compression]")
{
  CheckTrailingPartialCompressionUnit<Strategy::FULL_CACHE>();
}

TEST_CASE("A corrupt LZNT1 compression unit is rejected, not crashed on",
          "[attr-non-resident][compression]")
{
  CheckCorruptCompressedUnitIsRejected<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A corrupt LZNT1 compression unit is rejected, not crashed on "
    "(FULL_CACHE)",
    "[attr-non-resident][compression]")
{
  CheckCorruptCompressedUnitIsRejected<Strategy::FULL_CACHE>();
}

TEST_CASE("A compression unit no data run maps is rejected, not read as a hole",
          "[attr-non-resident][compression]")
{
  CheckUnmappedCompressionUnitIsRejected<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A compression unit no data run maps is rejected, not read as a hole "
    "(FULL_CACHE)",
    "[attr-non-resident][compression]")
{
  CheckUnmappedCompressionUnitIsRejected<Strategy::FULL_CACHE>();
}

TEST_CASE("A compression unit with real clusters after a hole is rejected",
          "[attr-non-resident][compression]")
{
  CheckRealClustersAfterHoleIsRejected<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A compression unit with real clusters after a hole is rejected "
    "(FULL_CACHE)",
    "[attr-non-resident][compression]")
{
  CheckRealClustersAfterHoleIsRejected<Strategy::FULL_CACHE>();
}

TEST_CASE("An interior compression unit that decompresses short is rejected",
          "[attr-non-resident][compression]")
{
  CheckShortDecompressedInteriorUnitIsRejected<Strategy::NO_CACHE>();
}

TEST_CASE(
    "An interior compression unit that decompresses short is rejected "
    "(FULL_CACHE)",
    "[attr-non-resident][compression]")
{
  CheckShortDecompressedInteriorUnitIsRejected<Strategy::FULL_CACHE>();
}

////////////////////////////////////////////////////////////////////////////
// Decompressed-unit caching
////////////////////////////////////////////////////////////////////////////

TEST_CASE(
    "FULL_CACHE serves a repeated read of the same compression unit from "
    "the decompressed-unit cache",
    "[attr-non-resident][compression]")
{
  ParsedRoot<Strategy::FULL_CACHE> root = ParseRoot<Strategy::FULL_CACHE>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCompressedFile());
  REQUIRE(root.record->ParseAttrs());

  std::optional<std::vector<BYTE>> first;
  const std::string firstTrace = CaptureTrace(
      [&]
      {
        first = ReadRootData<Strategy::FULL_CACHE>(
            *root.record, 0,
            NtfsBrowserTests::kXcaLznt1ExampleDecompressedSize);
      });
  REQUIRE(first.has_value());
  CHECK_THAT(firstTrace, Catch::Matchers::ContainsSubstring(
                             "Decompressed compression unit 0 into 142 bytes"));

  std::optional<std::vector<BYTE>> second;
  const std::string secondTrace = CaptureTrace(
      [&]
      {
        second = ReadRootData<Strategy::FULL_CACHE>(
            *root.record, 0,
            NtfsBrowserTests::kXcaLznt1ExampleDecompressedSize);
      });

  // Same bytes...
  REQUIRE(second.has_value());
  CHECK(*second == *first);
  // ...but served from the cache rather than decompressed again.
  CHECK_THAT(secondTrace, Catch::Matchers::ContainsSubstring(
                              "Compression unit 0 served from cache"));
  CHECK_THAT(secondTrace, !Catch::Matchers::ContainsSubstring(
                              "Decompressed compression unit 0"));
}

TEST_CASE(
    "NO_CACHE keeps no decompressed compression unit across ReadData calls",
    "[attr-non-resident][compression]")
{
  ParsedRoot<Strategy::NO_CACHE> root = ParseRoot<Strategy::NO_CACHE>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCompressedFile());
  REQUIRE(root.record->ParseAttrs());

  const auto read = [&]
  {
    return ReadRootData<Strategy::NO_CACHE>(
        *root.record, 0, NtfsBrowserTests::kXcaLznt1ExampleDecompressedSize);
  };

  std::optional<std::vector<BYTE>> first;
  const std::string firstTrace = CaptureTrace([&] { first = read(); });
  std::optional<std::vector<BYTE>> second;
  const std::string secondTrace = CaptureTrace([&] { second = read(); });

  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(*second == *first);
  CHECK_THAT(firstTrace, Catch::Matchers::ContainsSubstring(
                             "Decompressed compression unit 0 into 142 bytes"));
  CHECK_THAT(secondTrace,
             Catch::Matchers::ContainsSubstring(
                 "Decompressed compression unit 0 into 142 bytes"));
}

////////////////////////////////////////////////////////////////////////////
// What must NOT have changed
////////////////////////////////////////////////////////////////////////////

namespace
{

// Acceptance criterion: the existing 64-byte minimum total_size must still
// be accepted; the conditional CompressedSize field must not have grown it.
template <Strategy S>
void CheckMinimalNonResidentAttributeStillAccepted()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMinimalNonResidentData());

  std::string trace;
  bool parsed = false;
  trace = CaptureTrace([&] { parsed = root.record->ParseAttrs(); });

  CHECK(parsed);
  CHECK(root.record->getAttr(AttrType::DATA).size() == 1);
  CHECK_THAT(trace, !Catch::Matchers::ContainsSubstring(
                        "Attribute total_size too small for its header."));
  CHECK_THAT(trace, !Catch::Matchers::ContainsSubstring(
                        "Compressed attribute total_size too small for its "
                        "compressed size field."));
}

// Parses the root record of "image" with trace captured, and asserts it
// was rejected naming "message".
template <Strategy S>
void CheckCompressedHeaderRejected(std::vector<BYTE> image,
                                   const std::string& message)
{
  ParsedRoot<S> root = ParseRoot<S>(std::move(image));

  bool parsed = true;
  const std::string trace =
      CaptureTrace([&] { parsed = root.record->ParseAttrs(); });

  CHECK_FALSE(parsed);
  INFO("captured trace:\n" << trace);
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(message));
}

}  // namespace

TEST_CASE(
    "A minimum-size uncompressed non-resident attribute is still accepted",
    "[file-record][compression]")
{
  CheckMinimalNonResidentAttributeStillAccepted<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A minimum-size uncompressed non-resident attribute is still accepted "
    "(FULL_CACHE)",
    "[file-record][compression]")
{
  CheckMinimalNonResidentAttributeStillAccepted<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "A compressed non-resident attribute too small for its CompressedSize "
    "field is rejected",
    "[file-record][compression]")
{
  CheckCompressedHeaderRejected<Strategy::NO_CACHE>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithCompressedAttrMissingCompressedSize(),
      "Compressed attribute total_size too small for its compressed size "
      "field.");
}

TEST_CASE(
    "A compressed non-resident attribute too small for its CompressedSize "
    "field is rejected (FULL_CACHE)",
    "[file-record][compression]")
{
  CheckCompressedHeaderRejected<Strategy::FULL_CACHE>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithCompressedAttrMissingCompressedSize(),
      "Compressed attribute total_size too small for its compressed size "
      "field.");
}

TEST_CASE("An out-of-range comp_unit_size is rejected",
          "[attr-non-resident][compression]")
{
  CheckCompressedHeaderRejected<Strategy::NO_CACHE>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCompUnitSizeOutOfRange(),
      "Compression unit size is out of range.");
}

TEST_CASE("An out-of-range comp_unit_size is rejected (FULL_CACHE)",
          "[attr-non-resident][compression]")
{
  CheckCompressedHeaderRejected<Strategy::FULL_CACHE>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCompUnitSizeOutOfRange(),
      "Compression unit size is out of range.");
}

TEST_CASE("A compression unit larger than the supported maximum is rejected",
          "[attr-non-resident][compression]")
{
  CheckCompressedHeaderRejected<Strategy::NO_CACHE>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedCompressionUnit(),
      "Compression unit size is implausibly large.");
}

TEST_CASE(
    "A compression unit larger than the supported maximum is rejected "
    "(FULL_CACHE)",
    "[attr-non-resident][compression]")
{
  CheckCompressedHeaderRejected<Strategy::FULL_CACHE>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedCompressionUnit(),
      "Compression unit size is implausibly large.");
}

TEST_CASE("A misaligned compressed start_vcn is rejected",
          "[attr-non-resident][compression]")
{
  CheckCompressedHeaderRejected<Strategy::NO_CACHE>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMisalignedCompressedStartVcn(),
      "Compressed attribute start VCN is not compression unit aligned.");
}

TEST_CASE("A misaligned compressed start_vcn is rejected (FULL_CACHE)",
          "[attr-non-resident][compression]")
{
  CheckCompressedHeaderRejected<Strategy::FULL_CACHE>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMisalignedCompressedStartVcn(),
      "Compressed attribute start VCN is not compression unit aligned.");
}

////////////////////////////////////////////////////////////////////////////
// A compressed $INDEX_ALLOCATION (the shape the fuzz corpus entries use)
////////////////////////////////////////////////////////////////////////////

namespace
{

// Same fixture as NTFSLibTests/fuzz/data/compressed_index_allocation, but
// checked here on the actual traversal result, not just exit code/trace.
template <Strategy S>
void CheckCompressedIndexAllocationTraverses()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCompressedIndexAllocation());
  REQUIRE(root.record->ParseAttrs());
  CHECK(root.record->IsCompressed());
  CHECK(root.record->IsDirectory());

  std::vector<std::wstring> names;
  root.record->TraverseSubEntries(
      [](const IndexEntry& ie, void* context)
      {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            ie.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 1);
  CHECK(names[0] == NtfsBrowserTests::kCompressedIndexEntryName);

  const std::optional<IndexEntry> found =
      root.record->FindSubEntry(NtfsBrowserTests::kCompressedIndexEntryName);
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() ==
        NtfsBrowserTests::kCompressedIndexEntryMftRef);
}

// Malformed counterpart of the fixture above: corrupted LZNT1 bytes must
// yield an empty traversal and a trace, not a crash.
template <Strategy S>
void CheckCorruptCompressedIndexAllocationIsRejected()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithCorruptCompressedIndexAllocation());
  REQUIRE(root.record->ParseAttrs());

  size_t visited = 0;
  const std::string trace = CaptureTrace(
      [&]
      {
        root.record->TraverseSubEntries([](const IndexEntry&, void* context)
                                        { (*static_cast<size_t*>(context))++; },
                                        &visited);
      });

  CHECK(visited == 0);
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "LZNT1: back-reference before start of chunk."));
}

}  // namespace

TEST_CASE("A compressed $INDEX_ALLOCATION is decompressed and traversed",
          "[attr-index-alloc][compression]")
{
  CheckCompressedIndexAllocationTraverses<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A compressed $INDEX_ALLOCATION is decompressed and traversed "
    "(FULL_CACHE)",
    "[attr-index-alloc][compression]")
{
  CheckCompressedIndexAllocationTraverses<Strategy::FULL_CACHE>();
}

TEST_CASE("A corrupt compressed $INDEX_ALLOCATION yields no sub entries",
          "[attr-index-alloc][compression]")
{
  CheckCorruptCompressedIndexAllocationIsRejected<Strategy::NO_CACHE>();
}

TEST_CASE(
    "A corrupt compressed $INDEX_ALLOCATION yields no sub entries "
    "(FULL_CACHE)",
    "[attr-index-alloc][compression]")
{
  CheckCorruptCompressedIndexAllocationIsRejected<Strategy::FULL_CACHE>();
}

namespace
{

// One entry as a traversal reports it.
struct SeenEntry
{
  std::wstring name;
  ULONGLONG mft_ref;
  bool directory;
};

// Same fixture as NTFSLibTests/fuzz/data/surrogate_pair_names: the two
// resident entries come out first, then the two from the compressed block.
// Every name must survive as its own UTF-16 units and be found again by a
// lookup, which walks from the root into the compressed block.
template <Strategy S>
void CheckSurrogatePairNamesTraverse()
{
  ParsedRoot<S> root = ParseRoot<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSurrogatePairNames());
  REQUIRE(root.record->ParseAttrs());
  CHECK(root.record->IsCompressed());

  std::vector<SeenEntry> seen;
  root.record->TraverseSubEntries(
      [](const IndexEntry& ie, void* context)
      {
        static_cast<std::vector<SeenEntry>*>(context)->push_back(
            {std::wstring(ie.GetFilename()), ie.GetFileReference(),
             ie.IsDirectory()});
      },
      &seen);

  REQUIRE(seen.size() == NtfsBrowserTests::kSurrogateNames.size());
  for (size_t i = 0; i < seen.size(); i++)
  {
    INFO("entry " << i);
    CHECK(seen[i].name == NtfsBrowserTests::kSurrogateNames[i]);
    CHECK(seen[i].mft_ref == NtfsBrowserTests::kSurrogateNameMftRefs[i]);
    CHECK(seen[i].directory == NtfsBrowserTests::kSurrogateNameIsDirectory[i]);

    const std::optional<IndexEntry> found =
        root.record->FindSubEntry(NtfsBrowserTests::kSurrogateNames[i]);
    REQUIRE(found.has_value());
    CHECK(found->GetFileReference() ==
          NtfsBrowserTests::kSurrogateNameMftRefs[i]);
  }
}

}  // namespace

TEST_CASE("Names made of surrogate pairs are traversed and found",
          "[attr-index-alloc][compression]")
{
  CheckSurrogatePairNamesTraverse<Strategy::NO_CACHE>();
}

TEST_CASE("Names made of surrogate pairs are traversed and found (FULL_CACHE)",
          "[attr-index-alloc][compression]")
{
  CheckSurrogatePairNamesTraverse<Strategy::FULL_CACHE>();
}
