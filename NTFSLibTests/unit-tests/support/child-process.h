#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Runs exe with args, redirecting its stdout and stderr to two separate
// files instead of a pipe, and returns its exit code. Two temp files instead
// of two pipes: with a pipe, a stream nothing reads until the child exits
// can fill its buffer and deadlock the child. Throws when the child runs past
// one minute: it is killed first.
[[nodiscard]] int RunProcessToFiles(const std::filesystem::path& exe,
                                    const std::vector<std::wstring>& args,
                                    const std::filesystem::path& stdout_path,
                                    const std::filesystem::path& stderr_path);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
