#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/strategy.h>

using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::kFileRecordMagic;
using NtfsBrowser::Strategy;

// Regression fixture for bug F9: FileRecordHeader's ctor
// (src/data/file-record-header.cpp) used to require buffer.size() == 1024
// exactly, and FileRecordHeader::Data::raw was a fixed BYTE[1024] - a
// structural limit, since FileRecordHeaderImpl<FULL_CACHE>'s ctor
// memcpy()s buffer.data()/buffer.size() straight into &data_.raw[0]. A 4Kn
// (4096-byte-sector) NTFS volume necessarily has 4096-byte file records (a
// record can never be smaller than a sector), so every single 4Kn volume was
// unconditionally and permanently rejected.
//
// The fix widens raw[] to kMaxFileRecordSize (4096) and turns the ctor's
// hard equality check into a range check
// [kMinFileRecordHeaderSize, kMaxFileRecordSize]. That in turn opens a second
// latent bug: FileRecordHeader::HeaderCommon() used to bound offset_of_attr
// against sizeof(FileRecordHeader::Data::raw) - the union's static capacity
// - rather than the actual size the instance was constructed with. Once
// raw[] is widened, an ordinary 1024-byte record could let offset_of_attr
// point anywhere up to ~4088 without being rejected: FULL_CACHE would read
// uninitialized bytes (its by-value Data data_ only has buffer.size() bytes
// memcpy'd in), NO_CACHE would read genuinely out-of-bounds heap memory (its
// data_ is a std::span over the exact, buffer.size()-long real allocation).
// The fix stores the actual per-instance buffer size and bounds
// offset_of_attr against THAT instead.
//
// See F9 in docs/bug-reports/2026-09-03-full-repo.md.

namespace
{

// Builds a well-formed record header of exactly bufferSize bytes: valid
// magic, and offset_of_us/size_of_us describing the tightest-fitting Update
// Sequence Array that still lands entirely within the buffer (bufferSize /
// sectorSize sectors, one fixup WORD each, plus the leading USN itself) - the
// same "just fits" construction fake-ntfs-image.cpp uses (there hardcoded to
// a single sector per record; here generalized for a caller-chosen
// sectorSize). offset_of_attr is left at a safe, in-bounds value the caller
// can override.
std::vector<BYTE> MakeWellFormedBuffer(size_t bufferSize, size_t sectorSize,
                                       WORD offsetOfAttr)
{
  std::vector<BYTE> storage(bufferSize, 0);

  const size_t sectors = bufferSize / sectorSize;
  const WORD offsetOfUs = static_cast<WORD>(bufferSize - 2 * (1 + sectors));

  auto& header = *reinterpret_cast<FileRecordHeader::Data*>(storage.data());
  header.magic = kFileRecordMagic;
  header.offset_of_us = offsetOfUs;
  header.size_of_us = static_cast<WORD>(1 + sectors);
  header.offset_of_attr = offsetOfAttr;

  return storage;
}

}  // namespace

TEST_CASE(
    "FileRecordHeader must accept a well-formed 4096-byte buffer under "
    "NO_CACHE (4Kn volumes, F9)",
    "[file-record-header][regression]")
{
  constexpr size_t kBufferSize = 4096;
  constexpr size_t kSectorSize = 4096;

  const std::vector<BYTE> storage =
      MakeWellFormedBuffer(kBufferSize, kSectorSize, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  const auto fr =
      FileRecordHeader::Factory<Strategy::NO_CACHE>(buffer, kSectorSize);
  CHECK(fr.GetData()->magic == kFileRecordMagic);
}

TEST_CASE(
    "FileRecordHeader must accept a well-formed 4096-byte buffer under "
    "FULL_CACHE (4Kn volumes, F9)",
    "[file-record-header][regression]")
{
  // The FULL_CACHE case is exactly the one bug F9 calls out as structurally
  // risky: FileRecordHeaderImpl<FULL_CACHE>::data_ is a by-value
  // FileRecordHeader::Data, and its ctor memcpy()s buffer.data()/
  // buffer.size() straight into &data_.raw[0] - if raw[] were not widened to
  // at least 4096 bytes, this memcpy would overflow data_ by 3 KiB.
  constexpr size_t kBufferSize = 4096;
  constexpr size_t kSectorSize = 4096;

  const std::vector<BYTE> storage =
      MakeWellFormedBuffer(kBufferSize, kSectorSize, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  const auto fr =
      FileRecordHeader::Factory<Strategy::FULL_CACHE>(buffer, kSectorSize);
  CHECK(fr.GetData()->magic == kFileRecordMagic);
}

TEST_CASE(
    "FileRecordHeader must reject a buffer larger than kMaxFileRecordSize "
    "with a clear, specific message (F9)",
    "[file-record-header][regression]")
{
  constexpr size_t kTooBig = 8192;
  constexpr size_t kSectorSize = 4096;

  const std::vector<BYTE> storage =
      MakeWellFormedBuffer(kTooBig, kSectorSize, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  // Before the F9 fix: the ctor's hard `1024 != buffer.size()` check throws
  // the same generic "Buffer size of FileRecordHeader must be 1024." for
  // every wrong size, whether too small or (as here) implausibly large -
  // no distinct, actionable message. The fix must reject this buffer with a
  // message that specifically names the size problem (too large), not the
  // old one-size-fits-all wording.
  CHECK_THROWS_MATCHES(
      (FileRecordHeader::Factory<Strategy::NO_CACHE>(buffer, kSectorSize)),
      std::runtime_error,
      Catch::Matchers::MessageMatches(
          Catch::Matchers::ContainsSubstring("exceeds the maximum")));
}

TEST_CASE(
    "FileRecordHeader::HeaderCommon must bound offset_of_attr against this "
    "instance's own buffer size, not raw[]'s static capacity (F9)",
    "[file-record-header][regression]")
{
  // 2048 is deliberately NOT the classic 1024 size: before the F9 fix, the
  // ctor's hard `1024 != buffer.size()` check rejects a 2048-byte buffer
  // outright, regardless of offset_of_attr, so this test currently fails the
  // same way the two "well-formed 4096-byte buffer" cases above do - by
  // never reaching HeaderCommon() at all. After the F9 fix widens the
  // accepted range to [kMinFileRecordHeaderSize, kMaxFileRecordSize], this
  // buffer size becomes legal, which is exactly what exposes the second half
  // of the bug: a perfectly ordinary 2048-byte record - well within the
  // widened raw[] capacity (kMaxFileRecordSize == 4096) - whose
  // offset_of_attr points past its OWN actual size (2048) but still within
  // the union's static capacity. A naive fix that only widened raw[] and
  // left HeaderCommon() bounding against sizeof(FileRecordHeader::Data::raw)
  // would wrongly accept this and read past the real, exactly-2048-byte
  // allocation NO_CACHE's data_ span is backed by - the exact regression
  // this test exists to catch.
  constexpr size_t kDeclaredBufferSize = 2048;
  constexpr size_t kSectorSize = 2048;
  constexpr WORD kOffsetPastOwnSize = 3000;  // > 2048, but < 4096

  const std::vector<BYTE> storage = MakeWellFormedBuffer(
      kDeclaredBufferSize, kSectorSize, kOffsetPastOwnSize);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  auto fr = FileRecordHeader::Factory<Strategy::NO_CACHE>(buffer, kSectorSize);

  // After a correct fix: HeaderCommon() must bound offset_of_attr against
  // buffer_size_ (2048), not sizeof(Data::raw) (4096) - a fix that only
  // widened raw[] without also fixing HeaderCommon() would wrongly accept
  // this and build a pointer from &GetData()->raw[0] + 3000, 952 bytes past
  // the real, exactly-2048-byte-long backing allocation - NO_CACHE's data_
  // is a std::span over that exact allocation, so this would be a genuine
  // heap over-read, not just an indeterminate-value read. A correct fix must
  // reject this and return nullptr instead.
  CHECK(fr.HeaderCommon() == nullptr);
}
