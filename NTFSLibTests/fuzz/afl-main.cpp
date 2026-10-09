#include <ntfs-browser/win-types.h>

#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>  // IWYU pragma: keep
#include <ntfs-browser/log.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "gap-collation-probe.h"
#include "looping-disk-reader.h"
#include "named-stream-probe.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntryView;
namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::VolumeOptions;
using NtfsFuzz::gap_collation_search_name;
using NtfsFuzz::LoopingDiskReader;
using NtfsFuzz::named_data_stream_name;

namespace Mft = NtfsBrowser::Mft;
namespace Log = NtfsBrowser::Log;

// Windows gives a wmain() the command line as wide characters. A narrow
// main() only ever sees it through the active ANSI code page, which cannot
// express every path. Every other platform has narrow argv and nothing else.
#ifdef _WIN32
  #define NTFS_FUZZ_MAIN wmain
using ArgChar = wchar_t;
#else
  #define NTFS_FUZZ_MAIN main
using ArgChar = char;
#endif

namespace {

// Log::option_prefix in the character type this platform's argv has.
#ifdef _WIN32
constexpr std::wstring_view log_prefix = Log::option_prefix_w;
#else
constexpr std::string_view log_prefix = Log::option_prefix;
#endif

// Argument that turns on the read failure sweep, in argv's character type.
#ifdef _WIN32
constexpr std::wstring_view inject_option = L"--inject-read-failures";
#else
constexpr std::string_view inject_option = "--inject-read-failures";
#endif

// NtfsBpb::signature sits 3 bytes in, after the boot sector's jump instruction.
constexpr size_t bpb_signature_offset = 3;
// The exact bytes NtfsBpb::signature must hold to pass validation.
constexpr std::string_view bpb_signature = "NTFS    ";
// Byte length of bpb_signature, excluding its terminator.
constexpr size_t bpb_signature_len = 8;

// Overwrites the boot sector signature so ParseBootSector() accepts it.
void PatchBpbSignature(std::vector<BYTE>& data) {
  if (data.size() >= bpb_signature_offset + bpb_signature_len) {
    // The enclosing check leaves room for bpb_signature_len bytes at the
    // offset.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::memcpy(&data[bpb_signature_offset], bpb_signature.data(),
                bpb_signature_len);
  }
}

// How many successive ReadInto() calls get a one-shot injected failure,
// one run each. Covers the boot sector, $MFT and root record reads and the
// first index block reads. Later reads mostly repeat those code paths.
constexpr size_t injected_failure_runs = 16;

// The two VolumeOptions combinations every input is run under: strict
// (both flags off, the default) and fully recovering (both on). Exercises
// both the "reject the damaged item whole" and "salvage it" code paths.
constexpr std::array<VolumeOptions, 2> volume_option_modes{
    VolumeOptions{},
    VolumeOptions{.include_deleted = true, .recover_errors = true}};

// Opens the volume, parses the root file record, then walks its sub
// entries. A thrown exception counts as handled input rejection; only a
// real crash escapes, which AFL detects via this process's exit status.
//
// Templated on Strategy so the same input drives both NoCache and
// FullCache (see main()): some bugs only manifest in FullCache's object
// graph and are otherwise invisible to this fuzzer.
//
// failingRead makes that one ReadInto() call fail, exercising the
// disk-read error paths a looping reader never reaches on its own.
template <Cache::Strategy S>
void FuzzOnce(std::span<const BYTE> data, const VolumeOptions& options,
              std::optional<size_t> failing_read = {}) {
  const NtfsVolume<S> volume(
      std::make_unique<LoopingDiskReader>(data, failing_read), options);
  if (!volume.IsVolumeOK()) {
    return;
  }

  FileRecord file_record(volume);
  // Without DATA here, FindStream() below never sees a named $DATA
  // attribute on ROOT to walk. BITMAP and OBJECT_ID reach AttrBitmap and
  // the unhandled-attribute path of ParseAttr().
  file_record.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation |
                          Attr::Mask::Data | Attr::Mask::Bitmap |
                          Attr::Mask::ObjectId);
  if (!file_record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root))) {
    // file_record_ is guaranteed empty here, exercising IsDeleted()/
    // IsDirectory()'s guard against it.
    (void)file_record.IsDeleted();
    (void)file_record.IsDirectory();
    return;
  }
  if (!file_record.ParseAttrs()) {
    return;
  }

  // An empty callback is rejected up front, exercising that guard.
  file_record.TraverseAttrs(nullptr, nullptr);

  file_record.TraverseSubEntries([](const IndexEntryView&, void*) {}, nullptr);

  // FindStream() calls GetAttrName() on every named $DATA attribute it
  // walks, regardless of the name passed in.
  (void)file_record.FindStream(named_data_stream_name);

  // An empty name exercises FindStream()'s unnamed-stream branch, which the
  // call above (a fixed non-empty name) never reaches.
  (void)file_record.FindStream(L"");

  // Unlike TraverseSubEntries() above, FindSubEntry() actually compares
  // names, exercising a real B+-tree sub-node descent.
  (void)file_record.FindSubEntry(gap_collation_search_name);
}

// Runs FuzzOnce() and swallows any thrown exception: only a real crash
// may escape.
template <Cache::Strategy S>
void RunGuarded(std::span<const BYTE> data, const VolumeOptions& options,
                std::optional<size_t> failing_read = {}) {
  try {
    FuzzOnce<S>(data, options, failing_read);
  }
  // NOLINTNEXTLINE(bugprone-empty-catch)
  catch (const std::exception&) {
  }
  // NOLINTNEXTLINE(bugprone-empty-catch)
  catch (...) {
  }
}

// Prints command-line usage help.
void Usage(const ArgChar* program) {
  std::cerr << std::format(
      "usage: {} [--log=...] [--inject-read-failures] <input-file>\n",
      std::filesystem::path(program).string());
  std::cerr << std::format("  {}\n", Log::option_usage);
}

// Runs one AFL testcase file (the non-option argument) through the library
// once.
int Run(int argc, ArgChar** argv) {
  // Trace on the console by default, so an afl-fuzz run and the saved
  // regression corpus both keep producing every message without a flag.
  Log::Config log_config{.console_level = Log::Level::Trace};
  const std::span<ArgChar*> args(argv, gsl::narrow<size_t>(argc));
  const ArgChar* input = nullptr;
  bool inject_failures = false;

  for (size_t i = 1; i < args.size(); i++) {
    // i < args.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const ArgChar* const arg = args[i];
    if (std::basic_string_view<ArgChar>(arg) == inject_option) {
      inject_failures = true;
      continue;
    }
    if (std::basic_string_view<ArgChar>(arg).starts_with(log_prefix)) {
      if (!Log::ParseOption(arg, log_config)) {
        Usage(args.front());
        return 1;
      }
      continue;
    }
    if (input != nullptr) {
      Usage(args.front());
      return 1;
    }
    input = arg;
  }

  if (input == nullptr) {
    Usage(args.front());
    return 1;
  }

  if (!Log::Configure(log_config)) {
    std::cerr << std::format("Cannot open log file {}\n",
                             log_config.file_path.string());
  }

  std::optional<std::vector<BYTE>> data =
      LoopingDiskReader::LoadFile(std::filesystem::path(input));
  if (!data) {
    // Empty/unreadable testcase: nothing a looping reader could serve.
    return 0;
  }

  PatchBpbSignature(*data);

  // Guarded independently, so one run's exception can't skip the others.
  // Each strategy runs once per VolumeOptions mode, so both the strict
  // (reject-whole) and recovering (salvage) code paths are exercised.
  for (const VolumeOptions& options : volume_option_modes) {
    RunGuarded<Cache::Strategy::NoCache>(*data, options);
    RunGuarded<Cache::Strategy::FullCache>(*data, options);
  }

  if (!inject_failures) {
    return 0;
  }

  for (size_t failing_read = 0; failing_read < injected_failure_runs;
       ++failing_read) {
    for (const VolumeOptions& options : volume_option_modes) {
      RunGuarded<Cache::Strategy::NoCache>(*data, options, failing_read);
      RunGuarded<Cache::Strategy::FullCache>(*data, options, failing_read);
    }
  }

  return 0;
}

}  // namespace

// Keeps any exception from escaping main().
int NTFS_FUZZ_MAIN(int argc, ArgChar* argv[]) {
  try {
    return Run(argc, argv);
  } catch (...) {
    std::cerr << "Unhandled exception\n";
    return 1;
  }
}
