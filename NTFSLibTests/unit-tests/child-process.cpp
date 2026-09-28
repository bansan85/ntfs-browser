#include "child-process.h"

#include <array>
#include <stdexcept>

#ifdef _WIN32
  #include <ntfs-browser/win-types.h>
#else
  #include <cstring>

  #include <fcntl.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>

extern char** environ;
#endif

namespace fs = std::filesystem;

namespace NtfsBrowserTests
{

namespace
{

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

std::wstring BuildCommandLine(const fs::path& exe,
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
std::string ReadAllAndClose(HANDLE readPipe)
{
  std::string output;
  std::array<char, 4096> chunk{};
  DWORD bytesRead = 0;

  while (ReadFile(readPipe, chunk.data(), static_cast<DWORD>(chunk.size()),
                  &bytesRead, nullptr) &&
         bytesRead > 0)
  {
    output.append(chunk.data(), bytesRead);
  }

  CloseHandle(readPipe);
  return output;
}

#else

std::vector<std::string> NarrowArgs(const fs::path& exe,
                                    const std::vector<std::wstring>& args)
{
  std::vector<std::string> narrow;
  narrow.reserve(args.size() + 1);
  narrow.push_back(exe.string());
  for (const std::wstring& arg : args)
  {
    narrow.push_back(fs::path(arg).string());
  }
  return narrow;
}

std::vector<char*> ToArgv(std::vector<std::string>& narrowArgs)
{
  std::vector<char*> argv;
  argv.reserve(narrowArgs.size() + 1);
  for (std::string& arg : narrowArgs)
  {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return argv;
}

#endif

}  // namespace

ProcessOutput RunProcessCapturingOutput(const fs::path& exe,
                                        const std::vector<std::wstring>& args)
{
#ifdef _WIN32

  SECURITY_ATTRIBUTES pipeAttr{};
  pipeAttr.nLength = sizeof(pipeAttr);
  pipeAttr.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &pipeAttr, 0))
  {
    throw std::runtime_error("CreatePipe failed");
  }
  // Without this, the child would inherit the read end too, and could
  // deadlock ReadAllAndClose() below by keeping the write end open.
  if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0))
  {
    throw std::runtime_error("SetHandleInformation failed");
  }

  std::wstring cmdLine = BuildCommandLine(exe, args);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};

  const BOOL created = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr,
                                      TRUE, 0, nullptr, nullptr, &si, &pi);
  // Must close this process' copy now regardless of success, or
  // ReadAllAndClose() below hangs.
  CloseHandle(writePipe);
  if (!created)
  {
    CloseHandle(readPipe);
    throw std::runtime_error("CreateProcessW failed");
  }

  const std::string output = ReadAllAndClose(readPipe);

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return {static_cast<int>(exitCode), output};

#else

  std::array<int, 2> pipeFds{-1, -1};
  if (pipe(pipeFds.data()) != 0)
  {
    throw std::runtime_error("pipe failed");
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
  posix_spawn_file_actions_addclose(&actions, pipeFds[1]);

  std::vector<std::string> narrowArgs = NarrowArgs(exe, args);
  std::vector<char*> argv = ToArgv(narrowArgs);

  pid_t pid = 0;
  const int spawned =
      posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  // Must close this process' copy now regardless of success, or the read
  // loop below never sees EOF.
  close(pipeFds[1]);
  if (spawned != 0)
  {
    close(pipeFds[0]);
    throw std::runtime_error(std::string("posix_spawn failed: ") +
                             std::strerror(spawned));
  }

  std::string output;
  std::array<char, 4096> chunk{};
  ssize_t bytesRead = 0;
  while ((bytesRead = read(pipeFds[0], chunk.data(), chunk.size())) > 0)
  {
    output.append(chunk.data(), static_cast<std::size_t>(bytesRead));
  }
  close(pipeFds[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  return {exitCode, output};

#endif
}

int RunProcessToFiles(const fs::path& exe,
                      const std::vector<std::wstring>& args,
                      const fs::path& stdout_path, const fs::path& stderr_path)
{
#ifdef _WIN32

  SECURITY_ATTRIBUTES fileAttr{};
  fileAttr.nLength = sizeof(fileAttr);
  fileAttr.bInheritHandle = TRUE;

  const HANDLE outHandle =
      CreateFileW(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                  &fileAttr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  const HANDLE errHandle =
      CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                  &fileAttr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (outHandle == INVALID_HANDLE_VALUE || errHandle == INVALID_HANDLE_VALUE)
  {
    throw std::runtime_error("CreateFileW failed");
  }

  std::wstring cmdLine = BuildCommandLine(exe, args);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = outHandle;
  si.hStdError = errHandle;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};

  const BOOL created = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr,
                                      TRUE, 0, nullptr, nullptr, &si, &pi);
  CloseHandle(outHandle);
  CloseHandle(errHandle);
  if (!created)
  {
    throw std::runtime_error("CreateProcessW failed");
  }

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return static_cast<int>(exitCode);

#else

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);

  std::vector<std::string> narrowArgs = NarrowArgs(exe, args);
  std::vector<char*> argv = ToArgv(narrowArgs);

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
