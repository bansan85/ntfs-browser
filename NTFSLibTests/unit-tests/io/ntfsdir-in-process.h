#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/example/ntfsdir/arguments.h>
#include <ntfs-browser/example/ntfsdir/ntfsdir-core.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Test::Io {

// What one run of the NtfsDir logic gave, with no process: the exit code and
// the text of the two streams.
struct DirRun {
  int exit_code = 0;
  std::string out;
  std::string err;
};

// Reads what a binary stream holds, then closes it. The text has its carriage
// returns removed: a listing ends its lines with CR LF on Windows.
inline std::string TakeText(std::FILE* stream) {
  std::string text;
  std::rewind(stream);
  std::array<char, 4096> chunk{};
  while (const std::size_t got =
             std::fread(chunk.data(), 1, chunk.size(), stream)) {
    text.append(chunk.data(), got);
  }
  (void)std::fclose(stream);
  std::erase(text, '\r');
  return text;
}

// Parses args, the command line without the program name, which the test
// cannot go on without, and lists in this process.
inline DirRun ListInProcess(const std::vector<std::wstring>& args) {
  std::vector<std::wstring> words{L"ntfsdir"};
  words.insert(words.end(), args.begin(), args.end());
  std::vector<wchar_t*> pointers;
  for (std::wstring& word : words) {
    pointers.push_back(word.data());
  }
  std::wstring error;
  const auto parsed = NTFS_BROWSER_NAMESPACE::Example::NtfsDir::ParseArguments(
      static_cast<int>(pointers.size()), pointers.data(), error);
  INFO("parse error: " << std::filesystem::path(error).string());
  REQUIRE(parsed.has_value());

  std::FILE* out = std::tmpfile();
  std::FILE* err = std::tmpfile();
  REQUIRE(out != nullptr);
  REQUIRE(err != nullptr);
  DirRun run;
  run.exit_code =
      NTFS_BROWSER_NAMESPACE::Example::NtfsDir::ListTarget(*parsed, out, err);
  run.out = TakeText(out);
  run.err = TakeText(err);
  return run;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Io
