#include "child-process.h"

#include <chrono>
#include <cstdlib>
#include <stdexcept>

#include <sys/types.h>

#include <ntfs-browser/namespace.h>

#ifdef _WIN32
  #include <ntfs-browser/win-types.h>
#else
  #include <csignal>
  #include <cstring>
  #include <thread>

  #include <fcntl.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>

  #ifdef __APPLE__
// macOS headers do not declare environ.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern char** environ;
  #endif
#endif

namespace Fs = std::filesystem;

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Permissions of a redirect file the child's output is created with: rw-r--r--.
constexpr unsigned redirect_file_mode = 0644;

// How long a child may run before it is killed and the run fails. A hung
// child would otherwise hang ctest. A minute is far above what a test child
// needs, so a loaded machine does not trip it.
constexpr std::chrono::seconds child_time_limit{60};

#ifdef _WIN32

// Quotes one Windows command-line argument per the CRT's own argv parsing
// rules, so an embedded space, quote or backslash round-trips instead of
// splitting the argument early.
void AppendQuotedArg(std::wstring& cmd, const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd += arg;
    return;
  }

  cmd += L'"';
  for (auto it = arg.begin();; ++it) {
    std::size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++backslashes;
      ++it;
    }

    if (it == arg.end()) {
      cmd.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      cmd.append(backslashes * 2 + 1, L'\\');
      cmd += L'"';
    } else {
      cmd.append(backslashes, L'\\');
      cmd += *it;
    }
  }
  cmd += L'"';
}

std::wstring BuildCommandLine(const Fs::path& exe,
                              const std::vector<std::wstring>& args) {
  std::wstring cmd;
  AppendQuotedArg(cmd, exe.wstring());
  for (const std::wstring& arg : args) {
    cmd += L' ';
    AppendQuotedArg(cmd, arg);
  }
  return cmd;
}

// Startup info that hands the child out and err as its standard output and
// error, and this process' standard input.
STARTUPINFOW MakeStdioStartupInfo(HANDLE out, HANDLE err) {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out;
  si.hStdError = err;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  return si;
}

// Starts exe with args on the handles si names. Returns whether it started;
// on success pi holds its handles.
bool StartChild(const Fs::path& exe, const std::vector<std::wstring>& args,
                STARTUPINFOW& si, PROCESS_INFORMATION& pi) {
  std::wstring cmd_line = BuildCommandLine(exe, args);
  return CreateProcessW(nullptr, cmd_line.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &si, &pi) != FALSE;
}

// Waits for the child to exit, closes its handles, and returns its exit code.
// Kills the child and throws when it outlasts child_time_limit.
int WaitForChild(PROCESS_INFORMATION& pi) {
  const DWORD limit_ms = static_cast<DWORD>(
      std::chrono::duration_cast<std::chrono::milliseconds>(child_time_limit)
          .count());
  const bool exited =
      WaitForSingleObject(pi.hProcess, limit_ms) == WAIT_OBJECT_0;
  if (!exited) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
  }

  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (!exited) {
    throw std::runtime_error("child process ran past its time limit");
  }
  return static_cast<int>(exit_code);
}

#else

std::vector<std::string> NarrowArgs(const Fs::path& exe,
                                    const std::vector<std::wstring>& args) {
  std::vector<std::string> narrow;
  narrow.reserve(args.size() + 1);
  narrow.push_back(exe.string());
  for (const std::wstring& arg : args) {
    narrow.push_back(Fs::path(arg).string());
  }
  return narrow;
}

std::vector<char*> ToArgv(std::vector<std::string>& narrow_args) {
  std::vector<char*> argv;
  argv.reserve(narrow_args.size() + 1);
  for (std::string& arg : narrow_args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return argv;
}

// How often WaitForChild() looks whether the child has exited: short enough
// not to slow a test that spawns many children.
constexpr std::chrono::milliseconds child_poll_interval{2};

// Starts exe with args, its descriptors set up by actions. Returns
// posix_spawn's error number, zero on success, and stores the child's pid in
// pid.
int StartChild(const Fs::path& exe, const std::vector<std::wstring>& args,
               const posix_spawn_file_actions_t& actions, pid_t& pid) {
  std::vector<std::string> narrow_args = NarrowArgs(exe, args);
  std::vector<char*> argv = ToArgv(narrow_args);
  return posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(),
                     environ);
}

// Waits for the child to exit, and returns its exit code, or -1 if a signal
// ended it instead. Kills the child and throws when it outlasts
// child_time_limit.
int WaitForChild(pid_t pid) {
  const auto deadline = std::chrono::steady_clock::now() + child_time_limit;
  int status = 0;
  while (waitpid(pid, &status, WNOHANG) == 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      throw std::runtime_error("child process ran past its time limit");
    }
    std::this_thread::sleep_for(child_poll_interval);
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Throws the error of a child that posix_spawn could not start.
[[noreturn]] void ThrowSpawnFailed(int error) {
  throw std::runtime_error(std::string("posix_spawn failed: ") +
                           std::strerror(error));
}

#endif

}  // namespace

int RunProcessToFiles(const Fs::path& exe,
                      const std::vector<std::wstring>& args,
                      const Fs::path& stdout_path,
                      const Fs::path& stderr_path) {
#ifdef _WIN32

  SECURITY_ATTRIBUTES file_attr{};
  file_attr.nLength = sizeof(file_attr);
  file_attr.bInheritHandle = TRUE;

  const HANDLE out_handle =
      CreateFileW(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                  &file_attr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  const HANDLE err_handle =
      CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                  &file_attr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out_handle == INVALID_HANDLE_VALUE ||
      err_handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("CreateFileW failed");
  }

  STARTUPINFOW si = MakeStdioStartupInfo(out_handle, err_handle);
  PROCESS_INFORMATION pi{};
  const bool created = StartChild(exe, args, si, pi);
  CloseHandle(out_handle);
  CloseHandle(err_handle);
  if (!created) {
    throw std::runtime_error("CreateProcessW failed");
  }

  return WaitForChild(pi);

#else

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC,
                                   redirect_file_mode);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC,
                                   redirect_file_mode);

  pid_t pid = 0;
  const int spawned = StartChild(exe, args, actions, pid);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
    ThrowSpawnFailed(spawned);
  }

  return WaitForChild(pid);

#endif
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
