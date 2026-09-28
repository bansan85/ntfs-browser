#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace NtfsBrowserTests
{

// Exit code plus everything a child process wrote, stdout and stderr
// combined in the order it wrote them.
struct ProcessOutput
{
  int exit_code = 0;
  std::string output;
};

// Runs exe with args (each its own argv element; no shell involved), waits
// for it to exit, and captures its combined stdout/stderr through a pipe.
[[nodiscard]] ProcessOutput
    RunProcessCapturingOutput(const std::filesystem::path& exe,
                              const std::vector<std::wstring>& args);

// Runs exe with args, redirecting its stdout and stderr to two separate
// files instead of a pipe, and returns its exit code. Two temp files instead
// of two pipes: with a pipe, a stream nothing reads until the child exits
// can fill its buffer and deadlock the child.
[[nodiscard]] int RunProcessToFiles(const std::filesystem::path& exe,
                                    const std::vector<std::wstring>& args,
                                    const std::filesystem::path& stdout_path,
                                    const std::filesystem::path& stderr_path);

}  // namespace NtfsBrowserTests
