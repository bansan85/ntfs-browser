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
#include <windows.h>

#include <crtdbg.h>
#include <malloc.h>  // _resetstkoflw

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/log.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "sequential-disk-reader.h"

using namespace NtfsBrowser;
using NtfsBrowserTests::MakeGeneratorProducer;
using NtfsBrowserTests::SequentialDiskReader;

namespace
{

volatile bool g_stop = false;

// Set by OnInvalidParameter()/OnPureCall(), since neither raises a
// structured exception RunIteration()'s SEH handler could catch.
volatile bool g_crt_failure = false;

// Requests a graceful stop instead of an immediate process kill.
BOOL WINAPI OnConsoleEvent(DWORD /*eventType*/)
{
  g_stop = true;
  return TRUE;
}

// Debug-CRT invalid-parameter handler: without one, the CRT calls abort()
// directly, bypassing the SEH handler around FuzzOnce() with no diagnostic.
void OnInvalidParameter(const wchar_t* expr, const wchar_t* function,
                        const wchar_t* file, unsigned int line, uintptr_t)
{
  fwprintf(stderr, L"\nCRT invalid parameter: %ls in %ls (%ls:%u)\n",
           expr ? expr : L"?", function ? function : L"?", file ? file : L"?",
           line);
  fflush(stderr);
  g_crt_failure = true;
}

// Debug-CRT pure-call handler; same rationale as OnInvalidParameter.
void OnPureCall()
{
  fprintf(stderr, "\nPure virtual function called\n");
  fflush(stderr);
  g_crt_failure = true;
}

// Logs before letting std::terminate()'s default abort() proceed.
void OnTerminate()
{
  fprintf(stderr, "\nstd::terminate() called\n");
  fflush(stderr);
  std::abort();
}

// NtfsBpb::signature sits 3 bytes in, after the boot sector's jump instruction.
constexpr size_t kBpbSignatureOffset = 3;
// The exact bytes NtfsBpb::signature must hold to pass validation.
constexpr char kBpbSignature[] = "NTFS    ";
// Byte length of kBpbSignature, excluding its terminator.
constexpr size_t kBpbSignatureLen = 8;

// Serves an endless RNG stream, patching a valid boot-sector signature
// into the first read 95% of the time, so most iterations reach real
// MFT/attribute parsing instead of rejecting at ParseBootSector().
SequentialDiskReader::Producer
    MakeRandomProducer(std::mt19937_64::result_type seed)
{
  std::mt19937_64 rng(seed);
  const bool injectSignature =
      std::uniform_int_distribution<int>(1, 100)(rng) <= 95;

  return MakeGeneratorProducer(
      [rng, injectSignature, nthCall = 0](std::span<BYTE> dest) mutable
      {
        size_t filled = 0;
        while (filled < dest.size())
        {
          const uint64_t word = rng();
          const size_t chunk = std::min(sizeof(word), dest.size() - filled);
          std::memcpy(dest.data() + filled, &word, chunk);
          filled += chunk;
        }

        if (nthCall == 0 && injectSignature &&
            dest.size() >= kBpbSignatureOffset + kBpbSignatureLen)
        {
          std::memcpy(dest.data() + kBpbSignatureOffset, kBpbSignature,
                      kBpbSignatureLen);
        }
        nthCall++;
      });
}

// Opens the volume, parses the root file record, then walks its sub
// entries. A thrown exception counts as handled input rejection; only a
// real crash escapes, to the caller's SEH handler.
void FuzzOnce(unsigned seed)
{
  NtfsVolume<Strategy::NO_CACHE> volume(
      std::make_unique<SequentialDiskReader>(MakeRandomProducer(seed)));
  if (!volume.IsVolumeOK())
  {
    return;
  }

  FileRecord fr(volume);
  fr.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
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

  fr.TraverseSubEntries([](const IndexEntry&, void*) {}, nullptr);
}

// Swallows expected C++ exceptions; MSVC forbids mixing __try/__except
// with try/catch in the same function, so this stays below the SEH
// boundary in RunIteration().
void FuzzOnceCaught(unsigned seed)
{
  try
  {
    FuzzOnce(seed);
  }
  catch (const std::exception&)
  {
  }
  catch (...)
  {
  }
}

// Runs one seed under SEH and reports whether it crashed. Kept free of
// C++ objects needing unwinding, since MSVC forbids __try alongside that.
bool RunIteration(unsigned seed, DWORD& crashCode)
{
  crashCode = 0;
  g_crt_failure = false;

  __try
  {
    FuzzOnceCaught(seed);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    crashCode = GetExceptionCode();
    if (crashCode == EXCEPTION_STACK_OVERFLOW)
    {
      // Otherwise the consumed guard page never detects a later overflow.
      _resetstkoflw();
    }
    return false;
  }

  return !g_crt_failure;
}

}  // namespace

// With no args, fuzzes forever until Ctrl+C. With one numeric arg, fuzzes
// for that many iterations. With "--seed <seed>", replays one iteration.
int wmain(int argc, wchar_t* argv[])
{
  // Unbuffered, so an escaping CRT abort() can't strand output.
  setvbuf(stdout, nullptr, _IONBF, 0);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
  std::set_terminate(OnTerminate);

  // Sends debug-heap/assert failures to stderr instead of a blocking dialog.
  for (const int reportType : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT})
  {
    _CrtSetReportMode(reportType, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR);
  }

  // --log= options are consumed here; args holds everything else, so the
  // positional argument checks below stay as they were.
  Log::Config logConfig;
  std::vector<wchar_t*> args{argv[0]};
  for (int i = 1; i < argc; i++)
  {
    if (std::wstring_view(argv[i]).starts_with(Log::kOptionPrefixW))
    {
      if (!Log::ParseOption(argv[i], logConfig))
      {
        fprintf(stderr, "usage: %ls [--log=...] [iterations | --seed <seed>]\n",
                argv[0]);
        fprintf(stderr, "  %s\n", std::string(Log::kOptionUsage).c_str());
        return 1;
      }
      continue;
    }
    args.push_back(argv[i]);
  }
  if (!Log::Configure(logConfig))
  {
    fprintf(stderr, "Cannot open log file %ls\n", logConfig.file_path.c_str());
  }

  const size_t argCount = args.size();

  if (argCount == 3 && std::wcscmp(args[1], L"--seed") == 0)
  {
    const unsigned seed =
        static_cast<unsigned>(std::wcstoul(args[2], nullptr, 0));
    DWORD crashCode = 0;
    printf("Replaying seed=%u\n", seed);
    if (!RunIteration(seed, crashCode))
    {
      printf("CRASH (SEH 0x%08lX) seed=%u\n", crashCode, seed);
      return 1;
    }
    printf("No crash.\n");
    return 0;
  }

  std::optional<unsigned long long> maxIterations;
  if (argCount == 2)
  {
    maxIterations = std::wcstoull(args[1], nullptr, 0);
  }

  SetConsoleCtrlHandler(OnConsoleEvent, TRUE);

  std::random_device rd;
  unsigned long long iterations = 0;
  unsigned long long crashes = 0;

  printf(
      "Fuzzing NtfsVolume with an endless random stream. Press Ctrl+C to "
      "stop.\n");

  while (!g_stop && (!maxIterations || iterations < *maxIterations))
  {
    const unsigned seed = rd();
    ++iterations;

    // Printed before running, so an escaping crash still shows the seed.
    printf("seed=%u\r", seed);

    DWORD crashCode = 0;
    if (!RunIteration(seed, crashCode))
    {
      ++crashes;
      printf(
          "\nCRASH (SEH 0x%08lX) at iteration %llu, seed=%u -- repro with "
          "\"NtfsFuzzer --seed %u\"\n",
          crashCode, iterations, seed, seed);
    }

    if (iterations % 10000 == 0)
    {
      printf("iterations=%llu crashes=%llu\n", iterations, crashes);
      fflush(stdout);
    }
  }

  printf("Stopped after %llu iterations, %llu crash(es)\n", iterations,
         crashes);
  return crashes == 0 ? 0 : 1;
}
