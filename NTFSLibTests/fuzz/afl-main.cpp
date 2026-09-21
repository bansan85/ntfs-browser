#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/log.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "gap-collation-probe.h"
#include "looping-disk-reader.h"
#include "named-stream-probe.h"

using namespace NtfsBrowser;
using NtfsFuzz::kGapCollationSearchName;
using NtfsFuzz::kNamedDataStreamName;
using NtfsFuzz::LoopingDiskReader;

// Windows gives a wmain() the command line as wide characters. A narrow
// main() only ever sees it through the active ANSI code page, which cannot
// express every path. Every other platform has narrow argv and nothing else.
#ifdef _WIN32
  #define NTFS_FUZZ_MAIN wmain
  // printf conversion for a native argv or path string.
  #define NTFS_FUZZ_NATIVE "%ls"
using ArgChar = wchar_t;
#else
  #define NTFS_FUZZ_MAIN main
  // printf conversion for a native argv or path string.
  #define NTFS_FUZZ_NATIVE "%s"
using ArgChar = char;
#endif

namespace
{

// Log::kOptionPrefix in the character type this platform's argv has.
#ifdef _WIN32
constexpr std::wstring_view kLogPrefix = Log::kOptionPrefixW;
#else
constexpr std::string_view kLogPrefix = Log::kOptionPrefix;
#endif

// Argument that turns on the read failure sweep, in argv's character type.
#ifdef _WIN32
constexpr std::wstring_view kInjectOption = L"--inject-read-failures";
#else
constexpr std::string_view kInjectOption = "--inject-read-failures";
#endif

// NtfsBpb::signature sits 3 bytes in, after the boot sector's jump instruction.
constexpr size_t kBpbSignatureOffset = 3;
// The exact bytes NtfsBpb::signature must hold to pass validation.
constexpr char kBpbSignature[] = "NTFS    ";
// Byte length of kBpbSignature, excluding its terminator.
constexpr size_t kBpbSignatureLen = 8;

// Overwrites the boot sector signature so ParseBootSector() accepts it.
void PatchBpbSignature(std::vector<BYTE>& data)
{
  if (data.size() >= kBpbSignatureOffset + kBpbSignatureLen)
  {
    std::memcpy(data.data() + kBpbSignatureOffset, kBpbSignature,
                kBpbSignatureLen);
  }
}

// How many successive ReadInto() calls get a one-shot injected failure,
// one run each. Covers the boot sector, $MFT and root record reads and the
// first index block reads. Later reads mostly repeat those code paths.
constexpr size_t kInjectedFailureRuns = 16;

// Opens the volume, parses the root file record, then walks its sub
// entries. A thrown exception counts as handled input rejection; only a
// real crash escapes, which AFL detects via this process's exit status.
//
// Templated on Strategy so the same input drives both NO_CACHE and
// FULL_CACHE (see main()): some bugs only manifest in FULL_CACHE's object
// graph and are otherwise invisible to this fuzzer.
//
// failingRead makes that one ReadInto() call fail, exercising the
// disk-read error paths a looping reader never reaches on its own.
template <Strategy S>
void FuzzOnce(const std::vector<BYTE>& data,
              std::optional<size_t> failingRead = {})
{
  // Copied so both strategies replay the exact same bytes independently.
  NtfsVolume<S> volume(std::make_unique<LoopingDiskReader>(data, failingRead));
  if (!volume.IsVolumeOK())
  {
    return;
  }

  FileRecord fr(volume);
  // Without DATA here, FindStream() below never sees a named $DATA
  // attribute on ROOT to walk. BITMAP and OBJECT_ID reach AttrBitmap and
  // the unhandled-attribute path of ParseAttr().
  fr.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION | Mask::DATA |
                 Mask::BITMAP | Mask::OBJECT_ID);
  if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Enum::MftIdx::ROOT)))
  {
    // file_record_ is guaranteed empty here, exercising IsDeleted()/
    // IsDirectory()'s guard against it.
    (void)fr.IsDeleted();
    (void)fr.IsDirectory();
    return;
  }
  if (!fr.ParseAttrs())
  {
    return;
  }

  // An empty callback is rejected up front, exercising that guard.
  fr.TraverseAttrs(nullptr, nullptr);

  fr.TraverseSubEntries([](const IndexEntry&, void*) {}, nullptr);

  // FindStream() calls GetAttrName() on every named $DATA attribute it
  // walks, regardless of the name passed in.
  (void)fr.FindStream(kNamedDataStreamName);

  // Unlike TraverseSubEntries() above, FindSubEntry() actually compares
  // names, exercising a real B+-tree sub-node descent.
  (void)fr.FindSubEntry(kGapCollationSearchName);
}

// Runs FuzzOnce() and swallows any thrown exception: only a real crash
// may escape.
template <Strategy S>
void RunGuarded(const std::vector<BYTE>& data,
                std::optional<size_t> failingRead = {})
{
  try
  {
    FuzzOnce<S>(data, failingRead);
  }
  catch (const std::exception&)
  {
  }
  catch (...)
  {
  }
}

// Prints command-line usage help.
void Usage(const ArgChar* program)
{
  std::fprintf(stderr,
               "usage: " NTFS_FUZZ_NATIVE
               " [--log=...] [--inject-read-failures] <input-file>\n",
               program);
  std::fprintf(stderr, "  %s\n", std::string(Log::kOptionUsage).c_str());
}

}  // namespace

// Runs one AFL testcase file (the non-option argument) through the library
// once.
int NTFS_FUZZ_MAIN(int argc, ArgChar* argv[])
{
  // Trace on the console by default, so an afl-fuzz run and the saved
  // regression corpus both keep producing every message without a flag.
  Log::Config logConfig{.console_level = Log::Level::kTrace};
  const ArgChar* input = nullptr;
  bool injectFailures = false;

  for (int i = 1; i < argc; i++)
  {
    if (std::basic_string_view<ArgChar>(argv[i]) == kInjectOption)
    {
      injectFailures = true;
      continue;
    }
    if (std::basic_string_view<ArgChar>(argv[i]).starts_with(kLogPrefix))
    {
      if (!Log::ParseOption(argv[i], logConfig))
      {
        Usage(argv[0]);
        return 1;
      }
      continue;
    }
    if (input != nullptr)
    {
      Usage(argv[0]);
      return 1;
    }
    input = argv[i];
  }

  if (input == nullptr)
  {
    Usage(argv[0]);
    return 1;
  }

  if (!Log::Configure(logConfig))
  {
    std::fprintf(stderr, "Cannot open log file " NTFS_FUZZ_NATIVE "\n",
                 logConfig.file_path.c_str());
  }

  std::optional<std::vector<BYTE>> data =
      LoopingDiskReader::LoadFile(std::filesystem::path(input));
  if (!data)
  {
    // Empty/unreadable testcase: nothing a looping reader could serve.
    return 0;
  }

  PatchBpbSignature(*data);

  // Guarded independently, so one run's exception can't skip the others.
  RunGuarded<Strategy::NO_CACHE>(*data);
  RunGuarded<Strategy::FULL_CACHE>(*data);

  if (!injectFailures)
  {
    return 0;
  }

  for (size_t failingRead = 0; failingRead < kInjectedFailureRuns;
       ++failingRead)
  {
    RunGuarded<Strategy::NO_CACHE>(*data, failingRead);
    RunGuarded<Strategy::FULL_CACHE>(*data, failingRead);
  }

  return 0;
}
