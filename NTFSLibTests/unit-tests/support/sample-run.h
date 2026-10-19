#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// The exit code of a sample that ends with "return -1". Windows reports the
// int as it is; a POSIX process keeps its low 8 bits.
#ifdef _WIN32
inline constexpr int sample_failure_exit_code = -1;
#else
inline constexpr int sample_failure_exit_code = 255;
#endif

// Exit code and text of one sample run. The CRT writes CRLF on Windows, so
// the text has its carriage returns removed.
struct SampleRun {
  int exit_code = 0;
  std::string out;
  std::string err;
};

// Runs the sample exe with args, its stdout and stderr captured apart.
[[nodiscard]] SampleRun RunSample(const std::filesystem::path& exe,
                                  const std::vector<std::wstring>& args);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
