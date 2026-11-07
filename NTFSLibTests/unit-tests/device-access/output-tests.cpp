#include <algorithm>
#include <chrono>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
  #include <share.h>
  #include <sys/stat.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

#include <ntfs-browser/win-types.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/device-access/temp-dir.h>
#include <ntfs-browser/tool/device-access/line-buffer.h>
#include <ntfs-browser/tool/device-access/output.h>

namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

namespace TestDeviceAccess = NTFS_BROWSER_NAMESPACE::Test::DeviceAccess;
using ToolDeviceAccess::Output;

namespace {

#ifdef _WIN32
  #define NTFS_TEST_DUP _dup
  #define NTFS_TEST_DUP2 _dup2
  #define NTFS_TEST_CLOSE _close
  #define NTFS_TEST_FILENO _fileno
#else
  #define NTFS_TEST_DUP dup
  #define NTFS_TEST_DUP2 dup2
  #define NTFS_TEST_CLOSE close
  #define NTFS_TEST_FILENO fileno
#endif

// How long a test waits for the writer thread before it calls that a failure.
// Long on purpose: it is a bound on a bug, never a measure of speed.
constexpr std::chrono::seconds wait_limit{30};

// Sends the standard output to a file for the life of the object, and makes it
// write at once: the size of the file then tells what the writer thread wrote.
class StdoutToFile final {
 public:
  explicit StdoutToFile(const std::filesystem::path& path) : path_(path) {
    std::fflush(stdout);
    saved_ = NTFS_TEST_DUP(NTFS_TEST_FILENO(stdout));
    // The file exists before the first write: an empty one has size 0.
    std::ofstream(path_, std::ios::binary | std::ios::trunc).close();
#ifdef _WIN32
    int file = -1;
    _sopen_s(&file, path_.string().c_str(), _O_WRONLY | _O_BINARY | _O_APPEND,
             _SH_DENYNO, _S_IREAD | _S_IWRITE);
#else
    const int file = open(path_.c_str(), O_WRONLY | O_APPEND);
#endif
    REQUIRE(file >= 0);
    NTFS_TEST_DUP2(file, NTFS_TEST_FILENO(stdout));
    NTFS_TEST_CLOSE(file);
    unbuffered_ = std::setvbuf(stdout, nullptr, _IONBF, 0) == 0;
  }

  StdoutToFile(const StdoutToFile&) = delete;
  StdoutToFile& operator=(const StdoutToFile&) = delete;
  StdoutToFile(StdoutToFile&&) = delete;
  StdoutToFile& operator=(StdoutToFile&&) = delete;

  ~StdoutToFile() { Restore(); }

  // Gives the standard output back. Safe to call twice.
  void Restore() {
    if (saved_ < 0) {
      return;
    }
    std::fflush(stdout);
    NTFS_TEST_DUP2(saved_, NTFS_TEST_FILENO(stdout));
    NTFS_TEST_CLOSE(saved_);
    saved_ = -1;
    std::setvbuf(stdout, nullptr, _IOFBF, BUFSIZ);
  }

  // Whether every write of the process reaches the file at once.
  [[nodiscard]] bool Unbuffered() const { return unbuffered_; }

  // Bytes the file holds now.
  [[nodiscard]] std::uintmax_t Size() const {
    std::error_code ignored;
    const std::uintmax_t size = std::filesystem::file_size(path_, ignored);
    return ignored ? 0 : size;
  }

  // Waits until the file holds exactly size bytes.
  [[nodiscard]] bool WaitForSize(std::uintmax_t size) const {
    const auto limit = std::chrono::steady_clock::now() + wait_limit;
    while (std::chrono::steady_clock::now() < limit) {
      if (Size() == size) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
  }

  [[nodiscard]] std::string Content() const {
    std::ifstream in(path_, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

 private:
  std::filesystem::path path_;
  int saved_{-1};
  bool unbuffered_{false};
};

// A piece of 1000 bytes of the listing.
std::string Piece(char fill) { return std::string(1000, fill); }

// Writes lines of every kind of text the tools print, from number first up to
// but not including last, through out: an Output or a LineBuffer.
template <class Sink>
void FillListing(Sink& out, int first = 0, int last = 300) {
  for (int i = first; i < last; i++) {
    out.PrintLine("{:>6} {:<8} #{}", i, "name", i * 37);
    out.AppendNumber(static_cast<ULONGLONG>(i) * 1001, 12, '0');
    out.Append(" ");
    out.AppendRight("right", 9);
    out.AppendLeft("left", 7);
    out.AppendText(L"café \xD83D\xDE00 \xD800 end");
    out.EndLine();
  }
}

// Writes a listing of every kind of text the tools print, through an Output
// of flush_bytes, to the file at path. Returns what the file holds.
std::string Listing(const std::filesystem::path& path, size_t flush_bytes) {
  StdoutToFile redirect(path);
  {
    Output out(flush_bytes);
    FillListing(out);
  }
  redirect.Restore();
  return redirect.Content();
}

}  // namespace

TEST_CASE("Output gives the same bytes whatever the size of its chunks",
          "[device-access][output]") {
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path path = dir.Path() / "out.txt";

  const std::string whole = Listing(path, 0);
  REQUIRE_FALSE(whole.empty());
  CHECK(whole.starts_with(std::string("     0 name     #0") +
                          std::string(ToolDeviceAccess::line_end)));

  for (const size_t flush_bytes :
       {size_t{1}, size_t{7}, size_t{100}, size_t{4096}, size_t{100000},
        size_t{1} << 30U}) {
    INFO("flush_bytes " << flush_bytes);
    CHECK(Listing(path, flush_bytes) == whole);
  }
}

TEST_CASE("Output counts a size of 0 as the default size",
          "[device-access][output]") {
  const TestDeviceAccess::TempDir dir;
  StdoutToFile redirect(dir.Path() / "out.txt");
  if (!redirect.Unbuffered()) {
    SKIP("the standard output cannot be made to write at once");
  }

  std::uintmax_t before_end = 1;
  {
    Output out(0);
    for (int i = 0; i < 20; i++) {
      out.Print("{}", Piece('a'));
    }
    before_end = redirect.Size();
  }
  const std::uintmax_t after_end = redirect.Size();
  redirect.Restore();

  // 20 KB are far below the 1 MiB default: nothing was handed over.
  CHECK(before_end == 0);
  CHECK(after_end == 20000);
}

TEST_CASE("Output hands its writer a chunk as soon as it holds flush_bytes",
          "[device-access][output]") {
  const TestDeviceAccess::TempDir dir;
  StdoutToFile redirect(dir.Path() / "out.txt");
  if (!redirect.Unbuffered()) {
    SKIP("the standard output cannot be made to write at once");
  }

  // The assertions come after the standard output is given back: a reporter
  // that writes at once would write into the file.
  std::vector<std::uintmax_t> sizes;
  bool first_chunk = false;
  bool second_chunk = false;
  {
    Output out(4096);
    // Three pieces are under the size: the listing stays in memory.
    for (int i = 0; i < 3; i++) {
      out.Print("{}", Piece('a'));
    }
    sizes.push_back(redirect.Size());

    // The fourth piece is still under it; the fifth reaches it, and the five
    // pieces make the first chunk.
    out.Print("{}", Piece('b'));
    sizes.push_back(redirect.Size());
    out.Print("{}", Piece('c'));
    first_chunk = redirect.WaitForSize(5000);

    // Under the size again: nothing more is written until a chunk is full.
    for (int i = 0; i < 3; i++) {
      out.Print("{}", Piece('d'));
    }
    sizes.push_back(redirect.Size());
    out.Print("{}", Piece('e'));
    out.Print("{}", Piece('f'));
    second_chunk = redirect.WaitForSize(10000);
  }
  redirect.Restore();

  CHECK(sizes == std::vector<std::uintmax_t>{0, 0, 5000});
  CHECK(first_chunk);
  CHECK(second_chunk);
}

TEST_CASE("The first chunk of an Output ends at the first piece past its size",
          "[device-access][output]") {
  const TestDeviceAccess::TempDir dir;
  StdoutToFile redirect(dir.Path() / "out.txt");
  if (!redirect.Unbuffered()) {
    SKIP("the standard output cannot be made to write at once");
  }

  const size_t flush_bytes = GENERATE(size_t{2500}, size_t{9000});
  size_t printed = 0;
  bool silent_until_full = true;
  bool chunk_written = false;
  {
    Output out(flush_bytes);
    while (printed < flush_bytes) {
      silent_until_full = silent_until_full && redirect.Size() == 0;
      out.Print("{}", Piece('x'));
      printed += 1000;
    }
    // A chunk ends between two pieces of text, never inside one.
    chunk_written = redirect.WaitForSize(printed);
  }
  redirect.Restore();

  INFO("flush_bytes " << flush_bytes);
  CHECK(silent_until_full);
  CHECK(chunk_written);
}

TEST_CASE("Output flushes what is left when it ends, in any chunk size",
          "[device-access][output]") {
  const size_t flush_bytes = GENERATE(size_t{1}, size_t{4096}, size_t{0});
  INFO("flush_bytes " << flush_bytes);
  const TestDeviceAccess::TempDir dir;

  const std::string text = Listing(dir.Path() / "out.txt", flush_bytes);

  // 300 lines of the listing, the last one ended.
  CHECK(text.ends_with(ToolDeviceAccess::line_end));
  size_t lines = 0;
  for (size_t at = text.find(ToolDeviceAccess::line_end);
       at != std::string::npos;
       at = text.find(ToolDeviceAccess::line_end, at + 1)) {
    lines++;
  }
  CHECK(lines == 600);
}

TEST_CASE("A LineBuffer holds the text that an Output writes",
          "[device-access][output]") {
  const TestDeviceAccess::TempDir dir;
  ToolDeviceAccess::LineBuffer buffer;

  FillListing(buffer);

  CHECK(std::string(buffer.Text()) == Listing(dir.Path() / "out.txt", 0));
}

TEST_CASE("A LineBuffer forgets its text on Clear and builds on",
          "[device-access][output]") {
  ToolDeviceAccess::LineBuffer buffer;
  FillListing(buffer, 0, 10);
  const std::string first(buffer.Text());
  REQUIRE_FALSE(first.empty());

  buffer.Clear();
  CHECK(buffer.Text().empty());
  FillListing(buffer, 0, 10);

  CHECK(std::string(buffer.Text()) == first);
}

TEST_CASE("Output writes text built apart as it writes its own",
          "[device-access][output]") {
  const size_t flush_bytes = GENERATE(size_t{1}, size_t{700}, size_t{0});
  INFO("flush_bytes " << flush_bytes);
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path path = dir.Path() / "out.txt";
  const std::string whole = Listing(path, 0);

  StdoutToFile redirect(path);
  {
    Output out(flush_bytes);
    // Blocks of 40 lines, built apart.
    for (int first = 0; first < 300; first += 40) {
      ToolDeviceAccess::LineBuffer block;
      FillListing(block, first, (std::min)(first + 40, 300));
      out.AppendBlock(block.Text());
    }
  }
  redirect.Restore();

  CHECK(redirect.Content() == whole);
}
