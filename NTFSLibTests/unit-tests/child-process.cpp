#include "child-process.h"

#include <array>
#include <cstdlib>
#include <stdexcept>

#include <sys/types.h>

#ifdef _WIN32
  #include <ntfs-browser/win-types.h>

  #include <gsl/narrow>
#else
  #include <cstring>

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

namespace NtfsBrowserTests
{

namespace
{

// Bytes read from a child's pipe per read() call.
constexpr size_t pipe_chunk_size = 4096;

// Permissions of a redirect file the child's output is created with: rw-r--r--.
constexpr unsigned redirect_file_mode = 0644;

#ifdef _WIN32

// Quotes one Windows command-line argument per the CRT's own argv parsing
// rules, so an embedded space, quote or backslash round-trips instead of
// splitting the argument early.
void AppendQuotedArg(std::wstring& cmd, const std::wstring& arg)
{
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
  {
    cmd += arg;
    return;
  }

  cmd += L'"';
  for (auto it = arg.begin();; ++it)
  {
    std::size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\')
    {
      ++backslashes;
      ++it;
    }

    if (it == arg.end())
    {
      cmd.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"')
    {
      cmd.append(backslashes * 2 + 1, L'\\');
      cmd += L'"';
    }
    else
    {
      cmd.append(backslashes, L'\\');
      cmd += *it;
    }
  }
  cmd += L'"';
}

std::wstring BuildCommandLine(const Fs::path& exe,
                              const std::vector<std::wstring>& args)
{
  std::wstring cmd;
  AppendQuotedArg(cmd, exe.wstring());
  for (const std::wstring& arg : args)
  {
    cmd += L' ';
    AppendQuotedArg(cmd, arg);
  }
  return cmd;
}

// Reads a child process' pipe while waiting for it to exit. The write end
// must already be closed in this process -- otherwise ReadFile() blocks
// forever waiting for an EOF that can only come once every write handle
// (including this process' own copy) is gone.
std::string ReadAllAndClose(HANDLE read_pipe)
{
  std::string output;
  std::array<char, pipe_chunk_size> chunk{};
  DWORD bytes_read = 0;

  while (ReadFile(read_pipe, chunk.data(), gsl::narrow<DWORD>(chunk.size()),
                  &bytes_read, nullptr) &&
         bytes_read > 0)
  {
    output.append(chunk.data(), bytes_read);
  }

  CloseHandle(read_pipe);
  return output;
}

#else

std::vector<std::string> NarrowArgs(const Fs::path& exe,
                                    const std::vector<std::wstring>& args)
{
  std::vector<std::string> narrow;
  narrow.reserve(args.size() + 1);
  narrow.push_back(exe.string());
  for (const std::wstring& arg : args)
  {
    narrow.push_back(Fs::path(arg).string());
  }
  return narrow;
}

std::vector<char*> ToArgv(std::vector<std::string>& narrow_args)
{
  std::vector<char*> argv;
  argv.reserve(narrow_args.size() + 1);
  for (std::string& arg : narrow_args)
  {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return argv;
}

#endif

}  // namespace

ProcessOutput RunProcessCapturingOutput(const Fs::path& exe,
                                        const std::vector<std::wstring>& args)
{
#ifdef _WIN32

  SECURITY_ATTRIBUTES pipe_attr{};
  pipe_attr.nLength = sizeof(pipe_attr);
  pipe_attr.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &pipe_attr, 0))
  {
    throw std::runtime_error("CreatePipe failed");
  }
  // Without this, the child would inherit the read end too, and could
  // deadlock ReadAllAndClose() below by keeping the write end open.
  if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0))
  {
    throw std::runtime_error("SetHandleInformation failed");
  }

  std::wstring cmd_line = BuildCommandLine(exe, args);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};

  const BOOL created =
      CreateProcessW(nullptr, cmd_line.data(), nullptr, nullptr, TRUE, 0,
                     nullptr, nullptr, &si, &pi);
  // Must close this process' copy now regardless of success, or
  // ReadAllAndClose() below hangs.
  CloseHandle(write_pipe);
  if (!created)
  {
    CloseHandle(read_pipe);
    throw std::runtime_error("CreateProcessW failed");
  }

  const std::string output = ReadAllAndClose(read_pipe);

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return {static_cast<int>(exit_code), output};

#else

  std::array<int, 2> pipe_fds{-1, -1};
  if (pipe(pipe_fds.data()) != 0)
  {
    throw std::runtime_error("pipe failed");
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, std::get<1>(pipe_fds),
                                   STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, std::get<1>(pipe_fds),
                                   STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, std::get<0>(pipe_fds));
  posix_spawn_file_actions_addclose(&actions, std::get<1>(pipe_fds));

  std::vector<std::string> narrow_args = NarrowArgs(exe, args);
  std::vector<char*> argv = ToArgv(narrow_args);

  pid_t pid = 0;
  const int spawned =
      posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  // Must close this process' copy now regardless of success, or the read
  // loop below never sees EOF.
  close(std::get<1>(pipe_fds));
  if (spawned != 0)
  {
    close(std::get<0>(pipe_fds));
    throw std::runtime_error(std::string("posix_spawn failed: ") +
                             std::strerror(spawned));
  }

  std::string output;
  std::array<char, pipe_chunk_size> chunk{};
  ssize_t bytes_read = 0;
  while ((bytes_read =
              read(std::get<0>(pipe_fds), chunk.data(), chunk.size())) > 0)
  {
    output.append(chunk.data(), static_cast<std::size_t>(bytes_read));
  }
  close(std::get<0>(pipe_fds));

  int status = 0;
  waitpid(pid, &status, 0);
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  return {exit_code, output};

#endif
}

int RunProcessToFiles(const Fs::path& exe,
                      const std::vector<std::wstring>& args,
                      const Fs::path& stdout_path, const Fs::path& stderr_path)
{
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
  if (out_handle == INVALID_HANDLE_VALUE || err_handle == INVALID_HANDLE_VALUE)
  {
    throw std::runtime_error("CreateFileW failed");
  }

  std::wstring cmd_line = BuildCommandLine(exe, args);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out_handle;
  si.hStdError = err_handle;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};

  const BOOL created =
      CreateProcessW(nullptr, cmd_line.data(), nullptr, nullptr, TRUE, 0,
                     nullptr, nullptr, &si, &pi);
  CloseHandle(out_handle);
  CloseHandle(err_handle);
  if (!created)
  {
    throw std::runtime_error("CreateProcessW failed");
  }

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return static_cast<int>(exit_code);

#else

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC,
                                   redirect_file_mode);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC,
                                   redirect_file_mode);

  std::vector<std::string> narrow_args = NarrowArgs(exe, args);
  std::vector<char*> argv = ToArgv(narrow_args);

  pid_t pid = 0;
  const int spawned =
      posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0)
  {
    throw std::runtime_error(std::string("posix_spawn failed: ") +
                             std::strerror(spawned));
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;

#endif
}

}  // namespace NtfsBrowserTests
