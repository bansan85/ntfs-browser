#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "io/file-reader.h"

namespace {

// Size of the backing file: not a multiple of a sector, so the last read
// ends in the middle of one.
constexpr size_t file_size = 200777;

// Multiplier of the byte pattern, so a shifted read cannot match by accident.
constexpr size_t pattern_step = 7;

// Bits in a byte: the pattern mixes in the second byte of the index.
constexpr unsigned bits_per_byte = 8;

// A read that is neither sector aligned in offset nor in length.
constexpr size_t unaligned_offset = 513;
constexpr size_t unaligned_length = 777;

// A one-byte read at a one-byte offset.
constexpr size_t tiny_offset = 1;

// How far before the end of the file the end-of-file reads start.
constexpr size_t tail_length = 100;

// Bytes that depend on their offset.
std::vector<BYTE> MakeContent() {
  std::vector<BYTE> content(file_size);
  for (size_t i = 0; i < content.size(); i++) {
    // i < content.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    content[i] = static_cast<BYTE>(i * pattern_step + (i >> bits_per_byte));
  }
  return content;
}

// A byte range of the file, announced or read.
struct Range {
  size_t offset;
  size_t length;
};

// Deletes its file when it goes out of scope.
struct TempFile final {
  std::filesystem::path path;

  explicit TempFile(std::span<const BYTE> content) {
    std::random_device random_device;
    path = std::filesystem::temp_directory_path() /
           (L"ntfsbrowser-win32-reader-test-" +
            std::to_wstring(random_device()) + L".bin");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()),
              gsl::narrow<std::streamsize>(content.size()));
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;

  ~TempFile() { std::filesystem::remove(path); }
};

// A reader opened on a temporary file, with the content it was written from.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
class PrefetchFixture final {
  std::vector<BYTE> content_;
  TempFile file_;

 public:
  PrefetchFixture() : content_(MakeContent()), file_(content_) {
    REQUIRE(reader.Open(file_.path.wstring()));
  }

  // Announces every range to the reader.
  void Announce(std::initializer_list<Range> ranges) {
    for (const Range& range : ranges) {
      reader.Prefetch(range.offset, range.length);
    }
  }

  // Reads the length bytes at offset, and checks them against the file.
  void CheckRead(size_t offset, size_t length) {
    INFO("offset " << offset << " length " << length);
    std::vector<BYTE> dest(length);
    LARGE_INTEGER addr{.QuadPart = gsl::narrow<LONGLONG>(offset)};
    REQUIRE(reader.ReadInto(addr, dest));
    CHECK(std::equal(dest.begin(), dest.end(),
                     content_.begin() + gsl::narrow<std::ptrdiff_t>(offset)));
  }

  // Reads each range in turn, and checks it against the file.
  void CheckReads(std::initializer_list<Range> ranges) {
    for (const Range& range : ranges) {
      CheckRead(range.offset, range.length);
    }
  }

  // Declared after the file: it closes the file before the file is deleted.
  NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader;
};

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "Win32DiskReader reads exact bytes at unaligned offsets in any order",
    "[win32-disk-reader]", ((NTFS_BROWSER_NAMESPACE::Cache::Strategy S), S),
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache,
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache) {
  const std::vector<BYTE> content = MakeContent();
  const TempFile file(content);

  NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader;
  REQUIRE(reader.Open(file.path.wstring()));

  // A sequential scan first, then jumps backward and forward, then reads that
  // end exactly at the end of the file.
  const auto ranges =
      std::to_array<Range>({{0, 65536},
                            {65536, 65536},
                            {131072, 65536},
                            {unaligned_offset, unaligned_length},
                            {tiny_offset, 1},
                            {file_size - 65536 - 3, 65536 + 3 - 1},
                            {100001, 4096},
                            {file_size - tail_length, tail_length},
                            {file_size - 1, 1}});

  for (const Range& range : ranges) {
    INFO("offset " << range.offset << " length " << range.length);
    std::vector<BYTE> dest(range.length);
    LARGE_INTEGER addr{.QuadPart = gsl::narrow<LONGLONG>(range.offset)};
    REQUIRE(reader.ReadInto(addr, dest));
    CHECK(std::equal(dest.begin(), dest.end(),
                     content.begin() +
                         gsl::narrow<std::ptrdiff_t>(range.offset)));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "Win32DiskReader fails a read that runs past the end of the file",
    "[win32-disk-reader]", ((NTFS_BROWSER_NAMESPACE::Cache::Strategy S), S),
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache,
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache) {
  const TempFile file(MakeContent());

  NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader;
  REQUIRE(reader.Open(file.path.wstring()));

  std::vector<BYTE> dest(tail_length);

  LARGE_INTEGER straddling{
      .QuadPart = gsl::narrow<LONGLONG>(file_size - tail_length + 1)};
  CHECK_FALSE(reader.ReadInto(straddling, dest));

  LARGE_INTEGER beyond{.QuadPart = gsl::narrow<LONGLONG>(file_size)};
  CHECK_FALSE(reader.ReadInto(beyond, dest));

  // A failed read leaves the reader usable.
  LARGE_INTEGER inside{.QuadPart = gsl::narrow<LONGLONG>(unaligned_offset)};
  CHECK(reader.ReadInto(inside, dest));
}

TEMPLATE_TEST_CASE_SIG(
    "Win32DiskReader reads the same bytes with prefetch hints, whatever they "
    "are",
    "[win32-disk-reader][prefetch]",
    ((NTFS_BROWSER_NAMESPACE::Cache::Strategy S), S),
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache,
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache) {
  PrefetchFixture<S> fixture;

  SECTION("reads in the announced order, a range in several pieces") {
    fixture.Announce({{0, 65536},
                      {65536, 65536},
                      {unaligned_offset + 131072, unaligned_length}});
    fixture.CheckReads({{0, 4096},
                        {4096, 65536 - 4096},
                        {65536, 65536},
                        {unaligned_offset + 131072, unaligned_length}});
  }

  SECTION("a read past the first ranges, then reads of the skipped ones") {
    fixture.Announce({{0, 4096}, {8192, 4096}, {65536, 4096}});
    fixture.CheckReads({{65536, 4096}, {0, 4096}, {8192, 4096}});
  }

  SECTION("reads outside the ranges in between") {
    fixture.Announce({{65536, 65536}});
    fixture.CheckReads({{tiny_offset, 1},
                        {65536 + 100, 1000},
                        {file_size - tail_length, tail_length},
                        {65536 + 2000, 1000}});
  }

  SECTION("reads after the hints are cancelled") {
    fixture.Announce({{0, 65536}, {65536, 65536}});
    fixture.reader.CancelPrefetch();
    fixture.CheckReads({{0, 65536}, {65536, 65536}});
  }

  SECTION("more hints than are kept") {
    constexpr size_t hints = 2000;
    constexpr size_t hint_bytes = 64;
    for (size_t index = 0; index < hints; index++) {
      fixture.reader.Prefetch(index * hint_bytes, hint_bytes);
    }
    for (size_t index = 0; index < hints; index++) {
      fixture.CheckRead(index * hint_bytes, hint_bytes);
    }
  }

  SECTION("a range that runs past the end of the file") {
    fixture.reader.Prefetch(file_size - tail_length, 2 * tail_length);
    fixture.CheckRead(file_size - tail_length, tail_length);

    std::vector<BYTE> dest(2 * tail_length);
    fixture.reader.Prefetch(file_size - tail_length, dest.size());
    LARGE_INTEGER straddling{
        .QuadPart = gsl::narrow<LONGLONG>(file_size - tail_length)};
    CHECK_FALSE(fixture.reader.ReadInto(straddling, dest));
  }
}

TEMPLATE_TEST_CASE_SIG("Win32DiskReader reports the size of an ordinary file",
                       "[win32-disk-reader]",
                       ((NTFS_BROWSER_NAMESPACE::Cache::Strategy S), S),
                       NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache,
                       NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache) {
  const TempFile file(MakeContent());
  const TempFile empty(std::span<const BYTE>{});

  NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader;
  CHECK_FALSE(reader.Size().has_value());
  REQUIRE(reader.Open(file.path.wstring()));
  CHECK(reader.Size() == file_size);

  NTFS_BROWSER_NAMESPACE::Io::FileReader<S> empty_reader;
  REQUIRE(empty_reader.Open(empty.path.wstring()));
  CHECK(empty_reader.Size() == 0);
}
