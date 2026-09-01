// Feeds NtfsVolume/FileRecord an endless stream of random bytes and watches
// for crashes. Every read the library issues is answered with fresh garbage
// -- there is no real disk image, so nothing ever runs out of "data" the way
// a truncated file would.
//
// Usage:
//   NtfsFuzzer                 fuzz forever, until Ctrl+C
//   NtfsFuzzer <iterations>    fuzz for a bounded number of iterations
//   NtfsFuzzer --seed <seed>   replay a single iteration (crash repro)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <random>
#include <span>

#ifndef NOMINMAX
  #define NOMINMAX  // keep windows.h from clobbering std::min/std::max
#endif
#include <windows.h>

#include <crtdbg.h>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "sequential-disk-reader.h"

using namespace NtfsBrowser;
using NtfsBrowserTests::MakeGeneratorProducer;
using NtfsBrowserTests::SequentialDiskReader;

namespace
{

volatile bool g_stop = false;

BOOL WINAPI OnConsoleEvent(DWORD /*eventType*/)
{
  g_stop = true;
  return TRUE;
}

// The debug CRT's default reactions to these (invalid parameter, pure
// virtual call, terminate) call abort() directly rather than raising a
// structured exception, so they skip the SEH handler around FuzzOnce()
// entirely and would otherwise kill the fuzzer with no diagnostic at all.
void OnInvalidParameter(const wchar_t* expr, const wchar_t* function,
                        const wchar_t* file, unsigned int line, uintptr_t)
{
  fwprintf(stderr, L"\nCRT invalid parameter: %ls in %ls (%ls:%u)\n",
           expr ? expr : L"?", function ? function : L"?", file ? file : L"?",
           line);
  fflush(stderr);
}

void OnPureCall()
{
  fprintf(stderr, "\nPure virtual function called\n");
  fflush(stderr);
}

void OnTerminate()
{
  fprintf(stderr, "\nstd::terminate() called\n");
  fflush(stderr);
  std::abort();
}

// NtfsBpb::signature (src/data/ntfs-bpb.h) sits 3 bytes in, after the boot
// sector's jump instruction - not at offset 0.
constexpr size_t kBpbSignatureOffset = 3;
constexpr char kBpbSignature[] = "NTFS    ";
constexpr size_t kBpbSignatureLen = 8;

// Never signals EOF: every ReadInto() the library makes gets dest.size()
// more bytes out of the RNG, so a volume can be walked arbitrarily deep
// without the producer running dry. The very first read is the boot sector
// (see NtfsVolume::ParseBootSector), so 95% of the time its signature field
// is patched to a valid "NTFS    " - otherwise ParseBootSector() would reject
// almost every iteration before any MFT/attribute parsing gets exercised.
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

// Exercises the same path a real caller would: open the volume, parse the
// root file record, then walk its sub entries. Anything that throws here
// (bad_alloc, gsl::narrowing_error, ...) is a "handled" failure - genuine
// crashes (access violations, stack overflow, ...) are left to escape and
// are caught by the caller's SEH handler instead.
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
    return;
  }
  if (!fr.ParseAttrs())
  {
    return;
  }

  fr.TraverseSubEntries([](const IndexEntry&, void*) {}, nullptr);
}

// MSVC forbids mixing __try/__except with try/catch in the same function, so
// C++ exceptions (expected: garbage input rejected via a throw instead of a
// bool return) are swallowed here, one level below the SEH boundary.
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

// Kept free of C++ objects that need unwinding (MSVC forbids __try in a
// function that also has to run local destructors), so it does nothing but
// dispatch to FuzzOnceCaught() and translate whatever comes back.
bool RunIteration(unsigned seed, DWORD& crashCode)
{
  crashCode = 0;

  __try
  {
    FuzzOnceCaught(seed);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    crashCode = GetExceptionCode();
    return false;
  }

  return true;
}

}  // namespace

int main(int argc, char* argv[])
{
  // Unbuffered: a CRT-level abort() (debug heap corruption, /GS failure, ...)
  // bypasses the SEH handler below entirely, so anything only sitting in a
  // stdio buffer would be lost along with the process.
  setvbuf(stdout, nullptr, _IONBF, 0);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
  std::set_terminate(OnTerminate);

  // Debug-heap corruption / assert failures otherwise pop a blocking dialog
  // (or, headless, get treated as "Abort" and call exit(3) with zero
  // diagnostics). Route them to stderr instead so the fuzzer can log and
  // keep going.
  for (const int reportType : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT})
  {
    _CrtSetReportMode(reportType, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR);
  }

  if (argc == 3 && std::strcmp(argv[1], "--seed") == 0)
  {
    const unsigned seed =
        static_cast<unsigned>(std::strtoul(argv[2], nullptr, 0));
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
  if (argc == 2)
  {
    maxIterations = std::strtoull(argv[1], nullptr, 0);
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

    // Printed before running the seed (not after) and unbuffered, so a crash
    // that a debug-CRT abort() sneaks past the SEH handler below still
    // leaves the triggering seed visible in the output.
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
