#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
  // Keeps windows.h from clobbering std::min/std::max.
  #define NOMINMAX
#endif
#include <crtdbg.h>
#include <gsl/narrow>
#include <malloc.h>  // _resetstkoflw
#include <windows.h>

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "sequential-disk-reader.h"

using namespace NTFS_BROWSER_NAMESPACE;
using NTFS_BROWSER_NAMESPACE::Tests::MakeGeneratorProducer;
using NTFS_BROWSER_NAMESPACE::Tests::SequentialDiskReader;

namespace {

volatile bool g_stop = false;

// Set by OnInvalidParameter()/OnPureCall(), since neither raises a
// structured exception RunIteration()'s SEH handler could catch.
volatile bool g_crt_failure = false;

// Requests a graceful stop instead of an immediate process kill.
BOOL WINAPI OnConsoleEvent(DWORD /*eventType*/) {
  g_stop = true;
  return TRUE;
}

// Debug-CRT invalid-parameter handler: without one, the CRT calls abort()
// directly, bypassing the SEH handler around FuzzOnce() with no diagnostic.
void OnInvalidParameter(const wchar_t* expr, const wchar_t* function,
                        const wchar_t* file, unsigned int line, uintptr_t) {
  fwprintf(stderr, L"\nCRT invalid parameter: %ls in %ls (%ls:%u)\n",
           expr ? expr : L"?", function ? function : L"?", file ? file : L"?",
           line);
  fflush(stderr);
  g_crt_failure = true;
}

// Debug-CRT pure-call handler; same rationale as OnInvalidParameter.
void OnPureCall() {
  fprintf(stderr, "\nPure virtual function called\n");
  fflush(stderr);
  g_crt_failure = true;
}

// Logs before letting std::terminate()'s default abort() proceed.
void OnTerminate() {
  fprintf(stderr, "\nstd::terminate() called\n");
  fflush(stderr);
  std::abort();
}

// NtfsBpb::signature sits 3 bytes in, after the boot sector's jump instruction.
constexpr size_t bpb_signature_offset = 3;
// The exact bytes NtfsBpb::signature must hold to pass validation.
constexpr std::string_view bpb_signature = "NTFS    ";
// Byte length of bpb_signature, excluding its terminator.
constexpr size_t bpb_signature_len = 8;
// NtfsBpb::x_aa sits at the end of the 512-byte boot sector.
constexpr size_t bpb_marker_offset = 510;
// The two bytes of the 0xAA55 end marker, in disk order.
constexpr std::array<BYTE, 2> bpb_marker{0x55, 0xAA};

// Serves an endless RNG stream, patching a valid boot-sector signature
// into the first read 95% of the time, so most iterations reach real
// MFT/attribute parsing instead of rejecting at ParseBootSector().
SequentialDiskReader::Producer
    MakeRandomProducer(std::mt19937_64::result_type seed) {
  std::mt19937_64 rng(seed);
  const bool inject_signature =
      std::uniform_int_distribution<int>(1, 100)(rng) <= 95;

  return MakeGeneratorProducer(
      [rng, inject_signature, nth_call = 0](std::span<BYTE> dest) mutable {
        size_t filled = 0;
        while (filled < dest.size()) {
          const uint64_t word = rng();
          const size_t chunk = std::min(sizeof(word), dest.size() - filled);
          std::memcpy(dest.data() + filled, &word, chunk);
          filled += chunk;
        }

        if (nth_call == 0 && inject_signature &&
            dest.size() >= bpb_signature_offset + bpb_signature_len) {
          std::memcpy(dest.data() + bpb_signature_offset, bpb_signature.data(),
                      bpb_signature_len);
        }
        if (nth_call == 0 && inject_signature &&
            dest.size() >= bpb_marker_offset + bpb_marker.size()) {
          std::memcpy(dest.data() + bpb_marker_offset, bpb_marker.data(),
                      bpb_marker.size());
        }
        nth_call++;
      });
}

// The two VolumeOptions combinations every seed is run under: strict (both
// flags off, the default) and fully recovering (both on).
constexpr std::array<VolumeOptions, 2> volume_option_modes{
    VolumeOptions{},
    VolumeOptions{.include_deleted = true, .recover_errors = true}};

// Opens the volume, parses the root file record, then walks its sub
// entries. A thrown exception counts as handled input rejection; only a
// real crash escapes, to the caller's SEH handler. Runs the same
// deterministic byte stream (from seed) once per VolumeOptions mode, so
// both the strict and recovering code paths are exercised.
void FuzzOnce(unsigned seed) {
  for (const VolumeOptions& options : volume_option_modes) {
    NtfsVolume<Cache::Strategy::NoCache> volume(
        std::make_unique<SequentialDiskReader>(MakeRandomProducer(seed)),
        options);
    if (!volume.IsVolumeOK()) {
      continue;
    }

    NTFS_BROWSER_NAMESPACE::Io::FileRecord fr(volume);
    fr.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
    if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root))) {
      // file_record_ is guaranteed empty here, exercising IsDeleted()/
      // IsDirectory()'s guard against it.
      (void)fr.IsDeleted();
      (void)fr.IsDirectory();
      continue;
    }
    if (!fr.ParseAttrs()) {
      continue;
    }

    fr.TraverseSubEntries([](const IndexEntryView&, void*) {}, nullptr);
  }
}

// Swallows expected C++ exceptions; MSVC forbids mixing __try/__except
// with try/catch in the same function, so this stays below the SEH
// boundary in RunIteration().
void FuzzOnceCaught(unsigned seed) {
  try {
    FuzzOnce(seed);
  } catch (const std::exception&) {
  } catch (...) {
  }
}

// Runs one seed under SEH and reports whether it crashed. Kept free of
// C++ objects needing unwinding, since MSVC forbids __try alongside that.
bool RunIteration(unsigned seed, DWORD& crash_code) {
  crash_code = 0;
  g_crt_failure = false;

  __try {
    FuzzOnceCaught(seed);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    crash_code = GetExceptionCode();
    if (crash_code == EXCEPTION_STACK_OVERFLOW) {
      // Otherwise the consumed guard page never detects a later overflow.
      _resetstkoflw();
    }
    return false;
  }

  return !g_crt_failure;
}

// Routes CRT and terminate failures to the handlers above, and makes stdout
// unbuffered, so an escaping CRT abort() can't strand output.
void InstallFailureHandlers() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
  std::set_terminate(OnTerminate);

  // Sends debug-heap/assert failures to stderr instead of a blocking dialog.
  for (const int report_type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(report_type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(report_type, _CRTDBG_FILE_STDERR);
  }
}

// The parsed command line: the --log settings, and the operands that remain
// once those are removed. argv[0] is not an operand.
struct Arguments {
  Log::Config log_config;
  std::vector<wchar_t*> operands;
};

// Returns nullopt after printing usage for a malformed --log option.
std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[]) {
  Arguments args;
  for (int i = 1; i < argc; i++) {
    if (std::wstring_view(argv[i]).starts_with(Log::option_prefix_w)) {
      if (!Log::ParseOption(argv[i], args.log_config)) {
        fprintf(stderr, "usage: %ls [--log=...] [iterations | --seed <seed>]\n",
                argv[0]);
        fprintf(stderr, "  %s\n", std::string(Log::option_usage).c_str());
        return std::nullopt;
      }
      continue;
    }
    args.operands.push_back(argv[i]);
  }
  return args;
}

// Replays one seed, printing whether it crashes. Returns the exit code.
int ReplaySeed(unsigned seed) {
  DWORD crash_code = 0;
  printf("Replaying seed=%u\n", seed);
  if (!RunIteration(seed, crash_code)) {
    printf("CRASH (SEH 0x%08lX) seed=%u\n", crash_code, seed);
    return 1;
  }
  printf("No crash.\n");
  return 0;
}

// Fuzzes fresh random seeds until Ctrl+C, or for max_iterations seeds when
// one is given. Returns the exit code: 1 if any seed crashed.
int FuzzLoop(std::optional<unsigned long long> max_iterations) {
  SetConsoleCtrlHandler(OnConsoleEvent, TRUE);

  std::random_device rd;
  unsigned long long iterations = 0;
  unsigned long long crashes = 0;

  printf(
      "Fuzzing NtfsVolume with an endless random stream. Press Ctrl+C to "
      "stop.\n");

  while (!g_stop && (!max_iterations || iterations < *max_iterations)) {
    const unsigned seed = rd();
    ++iterations;

    // Printed before running, so an escaping crash still shows the seed.
    printf("seed=%u\r", seed);

    DWORD crash_code = 0;
    if (!RunIteration(seed, crash_code)) {
      ++crashes;
      printf(
          "\nCRASH (SEH 0x%08lX) at iteration %llu, seed=%u -- repro with "
          "\"NtfsFuzzer --seed %u\"\n",
          crash_code, iterations, seed, seed);
    }

    if (iterations % 10000 == 0) {
      printf("iterations=%llu crashes=%llu\n", iterations, crashes);
      fflush(stdout);
    }
  }

  printf("Stopped after %llu iterations, %llu crash(es)\n", iterations,
         crashes);
  return crashes == 0 ? 0 : 1;
}

}  // namespace

// With no args, fuzzes forever until Ctrl+C. With one numeric arg, fuzzes
// for that many iterations. With "--seed <seed>", replays one iteration.
// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  InstallFailureHandlers();

  const std::optional<Arguments> args = ParseArguments(argc, argv);
  if (!args) {
    return 1;
  }
  if (!Log::Configure(args->log_config)) {
    fprintf(stderr, "Cannot open log file %ls\n",
            args->log_config.file_path.c_str());
  }

  const std::vector<wchar_t*>& operands = args->operands;
  // operands.size() == 2 is tested first.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  if (operands.size() == 2 && std::wcscmp(operands[0], L"--seed") == 0) {
    return ReplaySeed(gsl::narrow<unsigned>(
        // operands.size() == 2 was tested above.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        std::wcstoul(operands[1], nullptr, 0)));
  }

  std::optional<unsigned long long> max_iterations;
  if (operands.size() == 1) {
    // operands.size() == 1 was tested above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    max_iterations = std::wcstoull(operands[0], nullptr, 0);
  }
  return FuzzLoop(max_iterations);
}
