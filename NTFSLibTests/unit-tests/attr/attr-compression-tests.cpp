// Tests for classic NTFS attribute-level compression
// (FILE_ATTRIBUTE_COMPRESSED plus a non-zero
// Data::HeaderNonResident::comp_unit_size, LZNT1-encoded compression units):
// the LZNT1 chunk decoder on its own (src/lznt1/decompress.h), then the full
// AttrNonResident<S>::ReadData() path over synthetic volumes, one case per
// per-unit encoding a real compressed attribute can use.
//
// Before this support existed, FileRecord<S>::ParseAttrs()
// (src/io/file-record.cpp) rejected any record whose STANDARD_INFORMATION/
// FILE_NAME flags marked it FILE_ATTRIBUTE_COMPRESSED ("Compressed and
// Encrypted file not supported yet !"), so every compressed file and
// directory on a real volume was silently lost - and even with that lifted,
// AttrNonResident never read comp_unit_size, so compressed clusters came
// back as raw, undecoded bytes.

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "compression-test-constants.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfs-browser/volume-options.h"
#include "optional-access.h"
#include "test-log-sink.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// An opened synthetic volume plus the parsed root FileRecord, kept together
// because the record's attributes reference both (the volume for reads, the
// record buffer for their own header bytes).
template <Cache::Strategy S>
struct ParsedRoot {
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>> record;
};

template <Cache::Strategy S>
ParsedRoot<S> ParseRoot(std::vector<BYTE> image,
                        NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  ParsedRoot<S> parsed;
  parsed.volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<Tests::MemoryDiskReader>(std::move(image)), options);
  REQUIRE(parsed.volume->IsVolumeOK());

  parsed.record = std::make_unique<NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>>(
      *parsed.volume);
  REQUIRE(
      parsed.record->ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  return parsed;
}

// ParseRoot() and then ParseAttrs() on the root record, which must succeed.
template <Cache::Strategy S>
ParsedRoot<S>
    ParseRootWithAttrs(std::vector<BYTE> image,
                       NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  ParsedRoot<S> parsed = ParseRoot<S>(std::move(image), options);
  REQUIRE(parsed.record->ParseAttrs());
  return parsed;
}

// Reads "size" bytes at "offset" of the root record's single $DATA
// attribute, truncated to what ReadData() actually produced; empty on failure.
template <Cache::Strategy S>
std::optional<std::vector<BYTE>>
    ReadRootData(const NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>& record,
                 ULONGLONG offset, size_t size) {
  const auto& data_attrs = record.GetAttr(Attr::Type::Data);
  REQUIRE(data_attrs.size() == 1);

  std::vector<BYTE> buffer(size, sentinel_byte);
  // The REQUIRE above checks the size of dataAttrs.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const std::optional<ULONGLONG> read = data_attrs[0]->ReadData(offset, buffer);
  if (!read) {
    return {};
  }

  buffer.resize(gsl::narrow<size_t>(*read));
  return buffer;
}

// Captures everything the library logs while body runs - the only
// externally visible signal that a cached decompression was reused. The
// library logger is already pinned to a trace-level capturing sink; see
// test-log-sink.h.
std::string CaptureTrace(const std::function<void()>& body) {
  (void)Tests::TakeCapturedLog();
  body();
  return Tests::TakeCapturedLog();
}

// Reads expected.size() bytes at offset of the root's $DATA, and requires
// exactly expected.
template <Cache::Strategy S>
void CheckRootBytes(const NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>& record,
                    ULONGLONG offset, const std::vector<BYTE>& expected) {
  const std::optional<std::vector<BYTE>> data =
      ReadRootData<S>(record, offset, expected.size());
  REQUIRE(data.has_value());
  REQUIRE(Tests::Unwrap(data).size() == expected.size());
  CHECK(std::memcmp(Tests::Unwrap(data).data(), expected.data(),
                    expected.size()) == 0);
}

// Reading size bytes at offset of the root's $DATA fails, and the trace says
// why.
template <Cache::Strategy S>
void CheckReadRejected(const NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>& record,
                       ULONGLONG offset, size_t size, const char* reason) {
  std::optional<std::vector<BYTE>> data;
  const std::string trace =
      CaptureTrace([&] { data = ReadRootData<S>(record, offset, size); });

  CHECK_FALSE(data.has_value());
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(reason));
}

}  // namespace

////////////////////////////////////////////////////////////////////////////
// End-to-end: AttrNonResident<S>::ReadData() over compressed attributes
////////////////////////////////////////////////////////////////////////////

namespace {

// I/O matrix row "Compressed unit": one real run of [MS-XCA] section 3.3
// LZNT1 bytes, then a sparse run padding out the full cluster count.
template <Cache::Strategy S>
void CheckCompressedFileReadsBackDecompressed() {
  ParsedRoot<S> root =
      ParseRoot<S>(Tests::BuildFakeNtfsImageWithCompressedFile());

  // Acceptance criterion: a compressed record is no longer skipped.
  REQUIRE(root.record->ParseAttrs());
  CHECK(root.record->IsCompressed());
  CHECK_FALSE(root.record->IsEncrypted());
  // real_size, not GetFileSize(): these minimal fixtures carry no $FILE_NAME.
  REQUIRE(root.record->GetAttr(Attr::Type::Data).size() == 1);
  // The REQUIRE above checks that there is one DATA attribute.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(root.record->GetAttr(Attr::Type::Data)[0]->GetDataSize() ==
        Tests::xca_lznt1_example_decompressed_size);

  const std::optional<std::vector<BYTE>> data = ReadRootData<S>(
      *root.record, 0, Tests::xca_lznt1_example_decompressed_size);
  REQUIRE(data.has_value());
  REQUIRE(Tests::Unwrap(data).size() ==
          Tests::xca_lznt1_example_decompressed_size);
  CHECK(std::memcmp(Tests::Unwrap(data).data(),
                    Tests::xca_lznt1_example_decompressed.data(),
                    Tests::Unwrap(data).size()) == 0);
}

// I/O matrix row "Stored (incompressible) unit": real runs fill the whole
// unit with no sparse pad, so the bytes must pass through undecompressed.
template <Cache::Strategy S>
void CheckStoredCompressionUnitReadsBackVerbatim() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithStoredCompressionUnit());

  CheckRootBytes<S>(
      *root.record, 0,
      Tests::CompressionFixturePattern(Tests::compression_unit_size));
}

// I/O matrix row "Fully sparse unit": no real cluster at all, so it must
// read back zero-filled, same as the pre-existing uncompressed sparse path.
template <Cache::Strategy S>
void CheckSparseCompressionUnitReadsBackZeroed() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithSparseCompressionUnit());

  const std::optional<std::vector<BYTE>> data =
      ReadRootData<S>(*root.record, 0, Tests::compression_unit_size);
  REQUIRE(data.has_value());
  REQUIRE(Tests::Unwrap(data).size() == Tests::compression_unit_size);

  size_t non_zero = 0;
  for (const BYTE byte_value : Tests::Unwrap(data)) {
    if (byte_value != 0) {
      non_zero++;
    }
  }
  CHECK(non_zero == 0);
}

// A unit whose compressed bytes span TWO non-contiguous data runs, which the
// read path must stitch together before decompressing.
template <Cache::Strategy S>
void CheckFragmentedCompressedFileReadsBackDecompressed() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithFragmentedCompressedFile());

  CheckRootBytes<S>(*root.record, 0,
                    Tests::CompressionFixturePattern(
                        Tests::fragmented_compressed_payload_size));
}

// I/O matrix row "Trailing partial unit at EOF": real_size is not a multiple
// of the compression unit size, so the trailing unit's real extent comes
// from a PARTIAL run. Reads must return exactly real_size bytes.
template <Cache::Strategy S>
void CheckTrailingPartialCompressionUnit() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithTrailingPartialCompressionUnit());

  const std::vector<BYTE> head = Tests::CompressionFixturePattern(
      Tests::trailing_partial_unit_stored_size);
  const std::vector<BYTE> tail =
      Tests::CompressionFixturePattern(Tests::trailing_partial_unit_tail_size);
  const size_t total = head.size() + tail.size();

  REQUIRE(root.record->GetAttr(Attr::Type::Data).size() == 1);
  // The REQUIRE above checks that there is one DATA attribute.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(root.record->GetAttr(Attr::Type::Data)[0]->GetDataSize() == total);

  // One read spanning both units: stored, then compressed trailing.
  const std::optional<std::vector<BYTE>> whole =
      ReadRootData<S>(*root.record, 0, total);
  REQUIRE(whole.has_value());
  REQUIRE(Tests::Unwrap(whole).size() == total);
  CHECK(std::memcmp(Tests::Unwrap(whole).data(), head.data(), head.size()) ==
        0);
  CHECK(std::memcmp(&Tests::Unwrap(whole).at(head.size()), tail.data(),
                    tail.size()) == 0);

  // Oversized read past EOF must truncate to the real count, not invent
  // bytes from sparse padding.
  const std::optional<std::vector<BYTE>> beyond =
      ReadRootData<S>(*root.record, head.size(), Tests::compression_unit_size);
  REQUIRE(beyond.has_value());
  REQUIRE(Tests::Unwrap(beyond).size() == tail.size());
  CHECK(std::memcmp(Tests::Unwrap(beyond).data(), tail.data(), tail.size()) ==
        0);
}

// I/O matrix row "Corrupt/truncated LZNT1 chunk": ReadData() must fail
// through its std::optional error channel, not crash or return garbage.
template <Cache::Strategy S>
void CheckCorruptCompressedUnitIsRejected() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithCorruptCompressedUnit());

  std::optional<std::vector<BYTE>> data;
  const std::string trace = CaptureTrace([&] {
    data = ReadRootData<S>(*root.record, 0,
                           Tests::xca_lznt1_example_decompressed_size);
  });

  CHECK_FALSE(data.has_value());
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "LZNT1: back-reference before start of chunk."));
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "Cannot decompress compression unit 0"));
}

// A unit no data run maps at all must make ReadData() fail, not hand back
// fabricated zeroes as if it were a hole.
template <Cache::Strategy S>
void CheckUnmappedCompressionUnitIsRejected() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithUnmappedCompressionUnit());

  // Unit 0 is well-formed and must still read back.
  CheckRootBytes<S>(
      *root.record, 0,
      Tests::CompressionFixturePattern(Tests::compression_unit_size));

  // Reading into unit 1 must not be answered with zeroes.
  CheckReadRejected<S>(*root.record, 0, 2 * Tests::compression_unit_size,
                       "is not fully mapped");
}

// A real run after a hole inside one unit is not an encoding this code
// understands: a compressed unit's sparse padding is always its tail.
template <Cache::Strategy S>
void CheckRealClustersAfterHoleIsRejected() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithRealClustersAfterHole());

  CheckReadRejected<S>(*root.record, 0, Tests::compression_unit_size,
                       "has real clusters after a hole");
}

// An interior unit whose LZNT1 stream ends early must not be zero-padded
// and returned as valid data (unlike a legitimate trailing partial unit).
template <Cache::Strategy S>
void CheckShortDecompressedInteriorUnitIsRejected() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithShortDecompressedUnit());

  CheckReadRejected<S>(*root.record, 0, Tests::compression_unit_size,
                       "decompressed to 100 bytes, expected at least 4096");
}

// A hole unit followed by a compressed unit: the hole must read back as zeroes
// and the compressed unit as its decoded bytes, also when one read straddles
// the boundary between them.
template <Cache::Strategy S>
void CheckSparseUnitBeforeCompressedUnitReadsBack() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithSparseThenCompressedUnit());
  REQUIRE(root.record->GetAttr(Attr::Type::Data).size() == 1);
  // The REQUIRE above checks that there is one DATA attribute.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(root.record->GetAttr(Attr::Type::Data)[0]->GetDataSize() ==
        Tests::sparse_then_compressed_real_size);

  constexpr size_t hole_size = Tests::compression_unit_size;
  constexpr size_t decoded_size = Tests::xca_lznt1_example_decompressed_size;

  std::optional<std::vector<BYTE>> whole;
  const std::string trace = CaptureTrace([&] {
    whole = ReadRootData<S>(*root.record, 0,
                            Tests::sparse_then_compressed_real_size);
  });
  REQUIRE(whole.has_value());
  REQUIRE(Tests::Unwrap(whole).size() ==
          Tests::sparse_then_compressed_real_size);
  const std::vector<BYTE>& bytes = Tests::Unwrap(whole);
  CHECK(std::all_of(bytes.begin(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(hole_size),
                    [](BYTE value) { return value == 0; }));
  CHECK(std::memcmp(&bytes.at(hole_size),
                    Tests::xca_lznt1_example_decompressed.data(),
                    decoded_size) == 0);
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "Compression unit 0 is sparse"));
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "Decompressed compression unit 1 into 142 bytes"));

  // Straddles the boundary: 100 zero bytes, then the decoded unit's first 100.
  constexpr size_t straddle_before = 100;
  constexpr size_t straddle_after = 100;
  const std::optional<std::vector<BYTE>> straddle =
      ReadRootData<S>(*root.record, hole_size - straddle_before,
                      straddle_before + straddle_after);
  REQUIRE(straddle.has_value());
  REQUIRE(Tests::Unwrap(straddle).size() == straddle_before + straddle_after);
  const std::vector<BYTE>& straddled = Tests::Unwrap(straddle);
  CHECK(std::all_of(straddled.begin(),
                    straddled.begin() +
                        static_cast<std::ptrdiff_t>(straddle_before),
                    [](BYTE value) { return value == 0; }));
  CHECK(std::memcmp(&straddled.at(straddle_before),
                    Tests::xca_lznt1_example_decompressed.data(),
                    straddle_after) == 0);

  // Reading only the compressed unit truncates at real_size, not at the unit.
  const std::optional<std::vector<BYTE>> decoded_only =
      ReadRootData<S>(*root.record, hole_size, hole_size);
  REQUIRE(decoded_only.has_value());
  CHECK(Tests::Unwrap(decoded_only).size() == decoded_size);
}

// A hole before a real cluster inside one unit is rejected, not read as a
// compressed unit with its hole moved.
template <Cache::Strategy S>
void CheckHoleBeforeRealClusterIsRejected() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithHoleBeforeRealCluster());

  CheckReadRejected<S>(*root.record, 0, Tests::compression_unit_size,
                       "has real clusters after a hole");
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("A compressed file's data reads back decompressed",
                       "[attr-non-resident][compression]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  CheckCompressedFileReadsBackDecompressed<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A stored (incompressible) compression unit reads back verbatim",
    "[attr-non-resident][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckStoredCompressionUnitReadsBackVerbatim<S>();
}

TEMPLATE_TEST_CASE_SIG("A fully sparse compression unit reads back zero-filled",
                       "[attr-non-resident][compression]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  CheckSparseCompressionUnitReadsBackZeroed<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A compression unit whose compressed bytes span several data runs is "
    "stitched back together before decompressing",
    "[attr-non-resident][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckFragmentedCompressedFileReadsBackDecompressed<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A trailing partial compression unit returns exactly real_size bytes",
    "[attr-non-resident][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckTrailingPartialCompressionUnit<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A corrupt LZNT1 compression unit is rejected, not crashed on",
    "[attr-non-resident][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckCorruptCompressedUnitIsRejected<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A compression unit no data run maps is rejected, not read as a hole",
    "[attr-non-resident][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckUnmappedCompressionUnitIsRejected<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A compression unit with real clusters after a hole is rejected",
    "[attr-non-resident][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckRealClustersAfterHoleIsRejected<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "An interior compression unit that decompresses short is rejected",
    "[attr-non-resident][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckShortDecompressedInteriorUnitIsRejected<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A hole unit before a compressed unit reads zero-filled, and a read "
    "straddling their boundary decodes the compressed side",
    "[attr-non-resident][compression][characterization]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  CheckSparseUnitBeforeCompressedUnitReadsBack<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A hole before a real cluster inside one compression unit is rejected",
    "[attr-non-resident][compression][characterization]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  CheckHoleBeforeRealClusterIsRejected<S>();
}

////////////////////////////////////////////////////////////////////////////
// Decompressed-unit caching
////////////////////////////////////////////////////////////////////////////

TEST_CASE(
    "FullCache serves a repeated read of the same compression unit from "
    "the decompressed-unit cache",
    "[attr-non-resident][compression]") {
  ParsedRoot<Cache::Strategy::FullCache> root =
      ParseRoot<Cache::Strategy::FullCache>(
          Tests::BuildFakeNtfsImageWithCompressedFile());
  REQUIRE(root.record->ParseAttrs());

  std::optional<std::vector<BYTE>> first;
  const std::string first_trace = CaptureTrace([&] {
    first = ReadRootData<Cache::Strategy::FullCache>(
        *root.record, 0, Tests::xca_lznt1_example_decompressed_size);
  });
  REQUIRE(first.has_value());
  CHECK_THAT(first_trace,
             Catch::Matchers::ContainsSubstring(
                 "Decompressed compression unit 0 into 142 bytes"));

  std::optional<std::vector<BYTE>> second;
  const std::string second_trace = CaptureTrace([&] {
    second = ReadRootData<Cache::Strategy::FullCache>(
        *root.record, 0, Tests::xca_lznt1_example_decompressed_size);
  });

  // Same bytes...
  REQUIRE(second.has_value());
  CHECK(Tests::Unwrap(second) == Tests::Unwrap(first));
  // ...but served from the cache rather than decompressed again.
  CHECK_THAT(second_trace, Catch::Matchers::ContainsSubstring(
                               "Compression unit 0 served from cache"));
  CHECK_THAT(second_trace, !Catch::Matchers::ContainsSubstring(
                               "Decompressed compression unit 0"));
}

TEST_CASE(
    "NoCache keeps no decompressed compression unit across ReadData calls",
    "[attr-non-resident][compression]") {
  ParsedRoot<Cache::Strategy::NoCache> root =
      ParseRoot<Cache::Strategy::NoCache>(
          Tests::BuildFakeNtfsImageWithCompressedFile());
  REQUIRE(root.record->ParseAttrs());

  const auto read = [&] {
    return ReadRootData<Cache::Strategy::NoCache>(
        *root.record, 0, Tests::xca_lznt1_example_decompressed_size);
  };

  std::optional<std::vector<BYTE>> first;
  const std::string first_trace = CaptureTrace([&] { first = read(); });
  std::optional<std::vector<BYTE>> second;
  const std::string second_trace = CaptureTrace([&] { second = read(); });

  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(Tests::Unwrap(second) == Tests::Unwrap(first));
  CHECK_THAT(first_trace,
             Catch::Matchers::ContainsSubstring(
                 "Decompressed compression unit 0 into 142 bytes"));
  CHECK_THAT(second_trace,
             Catch::Matchers::ContainsSubstring(
                 "Decompressed compression unit 0 into 142 bytes"));
}

////////////////////////////////////////////////////////////////////////////
// What must NOT have changed
////////////////////////////////////////////////////////////////////////////

namespace {

// Acceptance criterion: the existing 64-byte minimum total_size must still
// be accepted; the conditional CompressedSize field must not have grown it.
template <Cache::Strategy S>
void CheckMinimalNonResidentAttributeStillAccepted() {
  ParsedRoot<S> root =
      ParseRoot<S>(Tests::BuildFakeNtfsImageWithMinimalNonResidentData());

  std::string trace;
  bool parsed = false;
  trace = CaptureTrace([&] { parsed = root.record->ParseAttrs(); });

  CHECK(parsed);
  CHECK(root.record->GetAttr(Attr::Type::Data).size() == 1);
  CHECK_THAT(trace, !Catch::Matchers::ContainsSubstring(
                        "Attribute total_size too small for its header."));
  CHECK_THAT(trace, !Catch::Matchers::ContainsSubstring(
                        "Compressed attribute total_size too small for its "
                        "compressed size field."));
}

// Parses the root record of "image" with trace captured, and asserts it
// was rejected naming "message".
template <Cache::Strategy S>
void CheckCompressedHeaderRejected(std::vector<BYTE> image,
                                   const std::string& message) {
  ParsedRoot<S> root = ParseRoot<S>(std::move(image));

  bool parsed = true;
  const std::string trace =
      CaptureTrace([&] { parsed = root.record->ParseAttrs(); });

  CHECK_FALSE(parsed);
  INFO("captured trace:\n" << trace);
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(message));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A minimum-size uncompressed non-resident attribute is still accepted",
    "[file-record][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckMinimalNonResidentAttributeStillAccepted<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A compressed non-resident attribute too small for its CompressedSize "
    "field is rejected",
    "[file-record][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckCompressedHeaderRejected<S>(
      Tests::BuildFakeNtfsImageWithCompressedAttrMissingCompressedSize(),
      "Compressed attribute total_size too small for its compressed size "
      "field.");
}

TEMPLATE_TEST_CASE_SIG("An out-of-range comp_unit_size is rejected",
                       "[attr-non-resident][compression]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  CheckCompressedHeaderRejected<S>(
      Tests::BuildFakeNtfsImageWithCompUnitSizeOutOfRange(),
      "Compression unit size is out of range.");
}

TEMPLATE_TEST_CASE_SIG(
    "A compression unit larger than the supported maximum is rejected",
    "[attr-non-resident][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckCompressedHeaderRejected<S>(
      Tests::BuildFakeNtfsImageWithOversizedCompressionUnit(),
      "Compression unit size is implausibly large.");
}

TEMPLATE_TEST_CASE_SIG("A misaligned compressed start_vcn is rejected",
                       "[attr-non-resident][compression]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  CheckCompressedHeaderRejected<S>(
      Tests::BuildFakeNtfsImageWithMisalignedCompressedStartVcn(),
      "Compressed attribute start VCN is not compression unit aligned.");
}

////////////////////////////////////////////////////////////////////////////
// A compressed $INDEX_ALLOCATION (the shape the fuzz corpus entries use)
////////////////////////////////////////////////////////////////////////////

namespace {

// Same fixture as NTFSLibTests/fuzz/data/compressed_index_allocation, but
// checked here on the actual traversal result, not just exit code/trace.
template <Cache::Strategy S>
void CheckCompressedIndexAllocationTraverses() {
  // The named record is not built: only recover_errors lists this entry.
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithCompressedIndexAllocation(),
      {.recover_errors = true});
  CHECK(root.record->IsCompressed());
  CHECK(root.record->IsDirectory());

  std::vector<std::wstring> names;
  root.record->TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 1);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[0] == Tests::compressed_index_entry_name);

  const std::optional<IndexEntry> found =
      root.record->FindSubEntry(Tests::compressed_index_entry_name);
  REQUIRE(found.has_value());
  CHECK(Tests::Unwrap(found).GetFileReference() ==
        Tests::compressed_index_entry_mft_ref);
}

// Malformed counterpart of the fixture above: corrupted LZNT1 bytes must
// yield an empty traversal and a trace, not a crash.
template <Cache::Strategy S>
void CheckCorruptCompressedIndexAllocationIsRejected() {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(
      Tests::BuildFakeNtfsImageWithCorruptCompressedIndexAllocation());

  size_t visited = 0;
  const std::string trace = CaptureTrace([&] {
    root.record->TraverseSubEntries(
        [](const IndexEntryView&, void* context) {
          (*static_cast<size_t*>(context))++;
        },
        &visited);
  });

  CHECK(visited == 0);
  CHECK_THAT(trace, Catch::Matchers::ContainsSubstring(
                        "LZNT1: back-reference before start of chunk."));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A compressed $INDEX_ALLOCATION is decompressed and traversed",
    "[attr-index-alloc][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckCompressedIndexAllocationTraverses<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A corrupt compressed $INDEX_ALLOCATION yields no sub entries",
    "[attr-index-alloc][compression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckCorruptCompressedIndexAllocationIsRejected<S>();
}

namespace {

// One entry as a traversal reports it.
struct SeenEntry {
  std::wstring name;
  ULONGLONG mft_ref;
  bool directory;
};

// Same fixture as NTFSLibTests/fuzz/data/surrogate_pair_names: the two
// resident entries come out first, then the two from the compressed block.
// Every name must survive as its own UTF-16 units and be found again by a
// lookup, which walks from the root into the compressed block.
template <Cache::Strategy S>
void CheckSurrogatePairNamesTraverse() {
  // The named records are not built: only recover_errors lists these entries.
  ParsedRoot<S> root =
      ParseRootWithAttrs<S>(Tests::BuildFakeNtfsImageWithSurrogatePairNames(),
                            {.recover_errors = true});
  CHECK(root.record->IsCompressed());

  std::vector<SeenEntry> seen;
  root.record->TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<SeenEntry>*>(context)->push_back(
            {std::wstring(index_entry.GetFilename()),
             index_entry.GetFileReference(), index_entry.IsDirectory()});
      },
      &seen);

  REQUIRE(seen.size() == Tests::surrogate_names.size());
  for (size_t i = 0; i < seen.size(); i++) {
    INFO("entry " << i);
    // The REQUIRE above checks the size of seen.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(seen[i].name == Tests::surrogate_names[i]);
    // The surrogate* tables hold as many entries as surrogate_names.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(seen[i].mft_ref == Tests::surrogate_name_mft_refs[i]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(
        seen[i].directory ==
        // The surrogate* tables hold as many entries as surrogate_names.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        Tests::surrogate_name_is_directory[i]);

    const std::optional<IndexEntry> found =
        // The REQUIRE above makes seen as long as surrogate_names, like the
        // other tables.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        root.record->FindSubEntry(Tests::surrogate_names[i]);
    REQUIRE(found.has_value());
    CHECK(
        Tests::Unwrap(found).GetFileReference() ==
        // The surrogate* tables hold as many entries as surrogate_names.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        Tests::surrogate_name_mft_refs[i]);
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("Names made of surrogate pairs are traversed and found",
                       "[attr-index-alloc][compression]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  CheckSurrogatePairNamesTraverse<S>();
}

namespace {

// Reads one whole compression unit from the root's $DATA, whose run starts
// at an LCN that overflows. The read must fail without throwing, and the
// log must say why.
template <Cache::Strategy S>
void CheckOverflowingLcnUnitReadFails(std::vector<BYTE> image) {
  ParsedRoot<S> root = ParseRootWithAttrs<S>(std::move(image));

  std::optional<std::vector<BYTE>> read;
  const std::string trace = CaptureTrace([&] {
    REQUIRE_NOTHROW(
        read = ReadRootData(*root.record, 0, Tests::compression_unit_size));
  });

  CHECK_FALSE(read.has_value());
  CHECK_THAT(trace,
             Catch::Matchers::ContainsSubstring("byte address overflows"));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A stored compression unit whose LCN overflows a LONGLONG yields no data",
    "[attr-non-resident][compression][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckOverflowingLcnUnitReadFails<S>(
      Tests::BuildFakeNtfsImageWithStoredCompressionUnitBadLcn());
}

TEMPLATE_TEST_CASE_SIG(
    "A compressed compression unit whose LCN overflows a LONGLONG yields no "
    "data",
    "[attr-non-resident][compression][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckOverflowingLcnUnitReadFails<S>(
      Tests::BuildFakeNtfsImageWithCompressedCompressionUnitBadLcn());
}
