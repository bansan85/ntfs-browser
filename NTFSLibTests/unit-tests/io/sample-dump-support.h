#pragma once

// Fixture the ntfsdump and ntfsattr sample tests share.

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

namespace NTFS_BROWSER_NAMESPACE::Test::Io {

// The dump-tree image with its big file on the default run: 20 clusters at
// LCN 40, past the 16 KiB a dump shows.
inline std::vector<BYTE> DumpTreeImage() {
  return Test::FakeImage::BuildFakeNtfsImageWithDumpTree(
      Test::FakeImage::dump_big_file_size,
      {{.lcn = Test::FakeImage::dump_big_file_lcn,
        .clusters = Test::FakeImage::dump_big_file_clusters}});
}

// The argv of a command line: a program name, then args. It stays valid while
// the object lives.
class Argv final {
 public:
  Argv(std::initializer_list<std::wstring_view> args) : text_{L"tool"} {
    for (const std::wstring_view arg : args) {
      text_.emplace_back(arg);
    }
    for (std::wstring& arg : text_) {
      pointers_.push_back(arg.data());
    }
  }

  [[nodiscard]] int Count() const { return static_cast<int>(text_.size()); }

  [[nodiscard]] wchar_t** Data() { return pointers_.data(); }

 private:
  std::vector<std::wstring> text_;
  std::vector<wchar_t*> pointers_;
};

// What a run wrote to its two streams, and its exit code.
struct Captured {
  int exit_code = 0;
  std::string out;
  std::string err;
};

// Reads a whole file written through stream, which is open for update.
inline std::string ReadBack(std::FILE* stream) {
  std::string text;
  (void)std::fflush(stream);
  std::rewind(stream);
  std::array<char, 4096> chunk{};
  std::size_t count = 0;
  while ((count = std::fread(chunk.data(), 1, chunk.size(), stream)) > 0) {
    text.append(chunk.data(), count);
  }
  return text;
}

// Calls run with two binary temp files, one for each stream, and returns what
// they hold after it.
template <class Run>
Captured Capture(Run&& run) {
  std::random_device random_device;
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() /
      ("ntfs-example-" + std::to_string(random_device()));
  const std::filesystem::path out_path = base.string() + ".out";
  const std::filesystem::path err_path = base.string() + ".err";
  std::FILE* out = std::fopen(out_path.string().c_str(), "wb+");
  std::FILE* err = std::fopen(err_path.string().c_str(), "wb+");
  REQUIRE(out != nullptr);
  REQUIRE(err != nullptr);

  Captured captured;
  captured.exit_code = run(out, err);
  captured.out = ReadBack(out);
  captured.err = ReadBack(err);
  (void)std::fclose(out);
  (void)std::fclose(err);
  std::error_code ignored;
  std::filesystem::remove(out_path, ignored);
  std::filesystem::remove(err_path, ignored);
  return captured;
}

// text as narrow characters. The dumps are ASCII.
inline std::string Narrow(std::wstring_view text) {
  std::string narrow;
  for (const wchar_t character : text) {
    narrow += static_cast<char>(character);
  }
  return narrow;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Io
