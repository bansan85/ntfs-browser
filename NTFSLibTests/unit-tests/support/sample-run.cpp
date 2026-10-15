#include "sample-run.h"

#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "child-process.h"

namespace Fs = std::filesystem;

namespace NTFS_BROWSER_NAMESPACE::Tests {
namespace {

// Reads a whole file as text, without its carriage returns.
std::string ReadText(const Fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  std::erase(text, '\r');
  return text;
}

// A fresh temp path for one of the run's redirected streams.
Fs::path TempStreamPath(std::string_view tag) {
  std::random_device random_device;
  return Fs::temp_directory_path() /
         ("sample-run-" + std::to_string(random_device()) + "-" +
          std::string(tag) + ".txt");
}

}  // namespace

SampleRun RunSample(const Fs::path& exe,
                    const std::vector<std::wstring>& args) {
  const Fs::path out_path = TempStreamPath("out");
  const Fs::path err_path = TempStreamPath("err");
  SampleRun run;
  run.exit_code = RunProcessToFiles(exe, args, out_path, err_path);
  run.out = ReadText(out_path);
  run.err = ReadText(err_path);
  Fs::remove(out_path);
  Fs::remove(err_path);
  return run;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
