// The measuring tool of the benchmark. Subcommands:
//   replay    replays the read log of a run through the production reader
//   diskbench measures the disk, in the zone of its $MFT
//   simulate  scans a synthetic volume through a simulated disk
//   knobs     lists the tunable constants as JSON
//   disk-id   names the disk behind a target
//   purge     empties the system file cache
//   wake      reads a block to spin the disk up
//   sample    samples the disk counters until a process ends
// The Python script drives them; each prints JSON.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "bench-options.h"
#include "bench-tool.h"
#include "device-path.h"
#include "json.h"
#include "text-convert.h"

#ifndef _WIN32
  #include "argv-wide.h"
  #include "posix-device-disk-reader.h"
#endif

namespace NtfsBenchTool {

bool Arguments::Has(std::string_view name) const {
  for (const auto& [key, value] : options) {
    if (key == name) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> Arguments::Value(std::string_view name) const {
  for (const auto& [key, value] : options) {
    if (key == name) {
      return value;
    }
  }
  return std::nullopt;
}

std::uint64_t Arguments::Unsigned(std::string_view name, std::uint64_t fallback,
                                  bool& malformed) const {
  const std::optional<std::string> text = Value(name);
  if (!text) {
    return fallback;
  }
  std::uint64_t value = 0;
  const auto [end, error] =
      std::from_chars(text->data(), text->data() + text->size(), value);
  if (error != std::errc{} || end != text->data() + text->size()) {
    std::fprintf(stderr, "--%.*s expects a whole number\n",
                 static_cast<int>(name.size()), name.data());
    malformed = true;
    return fallback;
  }
  return value;
}

bool ParseArguments(const std::vector<std::wstring>& arguments,
                    Arguments& parsed) {
  for (const std::wstring& argument : arguments) {
    std::string error;
    switch (NtfsDevice::ParseBenchOption(argument, parsed.bench, error)) {
      case NtfsDevice::OptionResult::Accepted:
        continue;
      case NtfsDevice::OptionResult::Malformed:
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
      case NtfsDevice::OptionResult::NotMine:
        break;
    }
    if (argument.starts_with(L"--")) {
      const std::string text = NtfsDevice::WideToUtf8(argument);
      const std::size_t equals = text.find('=');
      parsed.options.emplace_back(
          text.substr(2, equals == std::string::npos ? std::string::npos
                                                     : equals - 2),
          equals == std::string::npos ? std::string()
                                      : text.substr(equals + 1));
      continue;
    }
    parsed.words.push_back(argument);
  }
  return true;
}

std::optional<NTFS_BROWSER_NAMESPACE::VolumeOptions>
    TunedOptions(const Arguments& args, std::wstring_view target) {
  NTFS_BROWSER_NAMESPACE::VolumeOptions options;
  NtfsDevice::AppliedTuning applied;
  const std::string failure =
      NtfsDevice::ApplyTuning(args.bench, target, options, applied);
  if (!failure.empty()) {
    std::fprintf(stderr, "%s\n", failure.c_str());
    return std::nullopt;
  }
  if (!args.bench.profile_path.empty() && !applied.section_found) {
    std::fprintf(stderr,
                 "The profile has no section for this disk: defaults used\n");
  }
  return options;
}

std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> OpenReader(
    std::wstring_view target,
    const std::array<NTFS_BROWSER_NAMESPACE::Tuning::ReaderTuning,
                     NTFS_BROWSER_NAMESPACE::Tuning::pattern_count>& caps) {
#ifdef _WIN32
  std::wstring path(target);
  if (const std::optional<wchar_t> letter = NtfsDevice::DriveLetterOf(target)) {
    path = std::wstring(L"\\\\.\\") + *letter + L":";
  }
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader =
      NTFS_BROWSER_NAMESPACE::Bench::OpenPlatformReader(path, caps);
#else
  static_cast<void>(caps);
  auto posix = std::make_unique<NtfsDevice::PosixDeviceDiskReader>();
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader;
  if (posix->Open(target)) {
    reader = std::move(posix);
  }
#endif
  if (!reader) {
    std::fprintf(stderr, "Cannot open %s\n",
                 NtfsDevice::WideToUtf8(target).c_str());
  }
  return reader;
}

int EmitJson(const Arguments& args, NtfsDevice::JsonWriter& json) {
  const std::string text = json.Take();
  std::optional<std::string> path = args.Value("json");
  if (!path) {
    path = args.Value("out");
  }
  if (path && !path->empty()) {
    const std::filesystem::path file(std::u8string(
        reinterpret_cast<const char8_t*>(path->data()), path->size()));
    std::ofstream out(NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(file),
                      std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
      std::fprintf(stderr, "Cannot write %s\n", path->c_str());
      return 1;
    }
    return 0;
  }
  std::fwrite(text.data(), 1, text.size(), stdout);
  return 0;
}

namespace {

void Usage() {
  std::printf(
      "Usage: ntfsbench <command> [options]\n"
      "  replay <readlog> <target> [--mode=recorded|ideal|both] [--purge]\n"
      "         [--profile=p] [--tune=n=v] [--json=out]\n"
      "                  replay the reads of a run through the reader\n"
      "  diskbench <target> [--zone=mft|offset:length] [--seconds=N]\n"
      "                  the disk's sheet: sequential and random reads by "
      "queue depth\n"
      "  simulate [--records=N] [--model=ssd|hdd] [--tune=n=v] [--json=out]\n"
      "                  scan a synthetic volume through a simulated disk\n"
      "  knobs           the tunable constants, as JSON\n"
      "  disk-id <target>  the disk's identity, as JSON\n"
      "  purge           empty the system file cache (administrator)\n"
      "  wake <target>   read one block, to spin the disk up\n"
      "  sample <target> --out=file (--pid=N | --seconds=N) "
      "[--interval-ms=100]\n"
      "                  sample the disk counters until the process ends\n");
}

int Run(const std::vector<std::wstring>& arguments) {
  if (arguments.empty()) {
    Usage();
    return 2;
  }
  const std::wstring& command = arguments.front();
  Arguments parsed;
  if (!ParseArguments({arguments.begin() + 1, arguments.end()}, parsed)) {
    return 2;
  }
  if (command == L"replay") {
    return ReplayCommand(parsed);
  }
  if (command == L"diskbench") {
    return DiskBenchCommand(parsed);
  }
  if (command == L"simulate") {
    return SimulateCommand(parsed);
  }
  if (command == L"knobs") {
    return KnobsCommand(parsed);
  }
  if (command == L"disk-id") {
    return DiskIdCommand(parsed);
  }
  if (command == L"purge") {
    return PurgeCommand(parsed);
  }
  if (command == L"wake") {
    return WakeCommand(parsed);
  }
  if (command == L"sample") {
    return SampleCommand(parsed);
  }
  Usage();
  return 2;
}

}  // namespace

}  // namespace NtfsBenchTool

#ifdef _WIN32

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return NtfsBenchTool::Run({argv + 1, argv + argc});
}

#else

int main(int argc, char* argv[]) {
  const std::vector<std::wstring> wide =
      NtfsDevice::ArgumentsAsWide(argc, argv);
  return NtfsBenchTool::Run({wide.begin() + 1, wide.end()});
}

#endif
