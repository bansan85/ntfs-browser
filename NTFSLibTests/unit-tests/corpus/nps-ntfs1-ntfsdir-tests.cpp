#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "nps-ntfs1-test-support.h"
#include "sample-run.h"

using NTFS_BROWSER_NAMESPACE::Tests::RunSample;
using NTFS_BROWSER_NAMESPACE::Tests::SampleRun;
namespace Fs = std::filesystem;

namespace {

// How much of ntfs1-gen2.raw the copy keeps. The $MFT sits near 172 MB and
// the directory indexes near 258 MB, while a listing never reads the file
// data after them. 250 MiB is the first round size past both.
constexpr std::size_t image_head_bytes = 250ULL * 1024 * 1024;

// Step of the scan for file records: a record starts on a sector.
constexpr std::size_t sector_bytes = 512;

// Size of one file record of this volume.
constexpr std::size_t record_bytes = 1024;

// Offset of the flags word in a file record header.
constexpr std::size_t record_flags_offset = 0x16;

// The in-use bit of those flags. A directory adds bit 2.
constexpr char record_in_use = 1;

// The file all three directories hold, in the form its $FILE_NAME stores.
constexpr std::string_view logo_name = "NIST_logo.jpg";

// A copy of the start of the image in a temp file, removed on exit.
struct ImageHead final {
  Fs::path path;
  std::vector<char> bytes;

  ImageHead() {
    std::ifstream in(NTFS_BROWSER_NAMESPACE::Tests::ntfs1_image,
                     std::ios::binary);
    REQUIRE(in.is_open());
    bytes.resize(image_head_bytes);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(in.gcount() == static_cast<std::streamsize>(bytes.size()));
    std::random_device random_device;
    path = Fs::temp_directory_path() /
           ("ntfsdir-nps-" + std::to_string(random_device()) + ".raw");
  }

  // Writes bytes to path, for NtfsDir to read.
  void Write() const {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
  }

  ~ImageHead() {
    std::error_code ignored;
    Fs::remove(path, ignored);
  }

  ImageHead(const ImageHead&) = delete;
  ImageHead& operator=(const ImageHead&) = delete;
  ImageHead(ImageHead&&) = delete;
  ImageHead& operator=(ImageHead&&) = delete;
};

// name as UTF-16LE, the way a $FILE_NAME holds it.
std::string Utf16Le(std::string_view name) {
  std::string wide;
  for (const char c : name) {
    wide.push_back(c);
    wide.push_back('\0');
  }
  return wide;
}

// Clears the in-use bit of the first record of a plain file (not a directory)
// that carries name, which frees it as a deletion does. Returns false when no
// record does.
bool FreeFirstFileNamed(std::vector<char>& image, std::string_view name) {
  const std::string wide = Utf16Le(name);
  for (std::size_t at = 0; at + record_bytes <= image.size();
       at += sector_bytes) {
    const std::string_view record(image.data() + at, record_bytes);
    // A plain file's flags are the in-use bit alone: 2 would make it a
    // directory.
    const bool plain_file =
        record.starts_with("FILE") &&
        static_cast<unsigned char>(record[record_flags_offset]) ==
            static_cast<unsigned char>(record_in_use) &&
        record[record_flags_offset + 1] == '\0';
    if (plain_file && record.find(wide) != std::string_view::npos) {
      image[at + record_flags_offset] = 0;
      return true;
    }
  }
  return false;
}

// How many times needle occurs in text.
std::size_t Count(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = text.find(needle); at != std::string_view::npos;
       at = text.find(needle, at + needle.size())) {
    count++;
  }
  return count;
}

// The part of listing that follows the header of directory, up to the next
// header.
std::string_view Block(std::string_view listing, std::string_view directory) {
  const std::string header = "\nDirectory of ";
  for (std::size_t at = listing.find(header); at != std::string_view::npos;
       at = listing.find(header, at + header.size())) {
    const std::size_t line_end = listing.find('\n', at + 1);
    const std::string_view title = listing.substr(at, line_end - at);
    if (title.ends_with(directory)) {
      const std::size_t next = listing.find(header, line_end);
      return listing.substr(line_end, next - line_end);
    }
  }
  return {};
}

// The end of the line of logo_name in a listing that shows deleted items,
// when the file is deleted: the marker column holds a star.
constexpr std::string_view deleted_logo_line = "\t* NIST_logo.jpg\n";

// The same line for a file that exists: the marker column holds a space.
constexpr std::string_view live_logo_line = "\t  NIST_logo.jpg\n";

}  // namespace

TEST_CASE("NtfsDir --deleted marks no file of the intact NPS ntfs1 image",
          "[nps][integration]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(
      NTFS_BROWSER_NAMESPACE::Tests::ntfs1_image);

  const SampleRun run = RunSample(
      NTFS_DIR_EXE, {L"--recursive", L"--deleted",
                     NTFS_BROWSER_NAMESPACE::Tests::ntfs1_image.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out.ends_with("Files: 19, Directories: 4\n"));
  CHECK(Count(run.out, deleted_logo_line) == 0);
  CHECK(Count(run.out, live_logo_line) == 3);
}

TEST_CASE("NtfsDir --deleted shows a file freed in the NPS ntfs1 image",
          "[nps][integration]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(
      NTFS_BROWSER_NAMESPACE::Tests::ntfs1_image);

  ImageHead image;
  REQUIRE(FreeFirstFileNamed(image.bytes, logo_name));
  image.Write();

  const SampleRun strict =
      RunSample(NTFS_DIR_EXE, {L"--recursive", image.path.wstring()});
  const SampleRun deleted = RunSample(
      NTFS_DIR_EXE, {L"--recursive", L"--deleted", image.path.wstring()});
  const SampleRun recover = RunSample(
      NTFS_DIR_EXE, {L"--recursive", L"--recover", image.path.wstring()});

  CHECK(strict.exit_code == 0);
  CHECK(strict.out.ends_with("Files: 18, Directories: 4\n"));
  CHECK(Count(strict.out, logo_name) == 2);
  CHECK(Count(strict.out, "\t* ") == 0);

  CHECK(deleted.exit_code == 0);
  CHECK(deleted.out.ends_with("Files: 19, Directories: 4\n"));
  CHECK(Count(deleted.out, deleted_logo_line) == 1);
  CHECK(Count(deleted.out, live_logo_line) == 2);
  // The file freed is the first of the three in the $MFT, Compressed's.
  CHECK(Count(Block(deleted.out, "Compressed"), deleted_logo_line) == 1);
  CHECK(Count(Block(deleted.out, "Encrypted"), live_logo_line) == 1);
  CHECK(Count(Block(deleted.out, "RAW"), live_logo_line) == 1);

  CHECK(recover.exit_code == 0);
  CHECK(recover.out == strict.out);
}
