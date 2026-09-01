// Feeds NtfsVolume/FileRecord the content of an AFL testcase file, for use
// with classic afl-gcc/afl-g++ (compile-time instrumentation, one run per
// process invocation -- no persistent mode, unlike afl++).
//
// Usage (from afl-fuzz):
//   afl-fuzz -i in -o out -- ./NtfsFuzzerAfl @@
//
// Unlike the RNG-backed clang fuzzer (main.cpp), reads never signal EOF by
// generating fresh data -- they loop back to the start of the input file
// instead (see LoopingDiskReader), so AFL's mutations of a small testcase
// can still drive parsing arbitrarily deep. The BPB signature is always
// patched in (no probabilistic skip): AFL cannot discover an 8-byte magic
// constant via bit flips alone, and every run should get past
// NtfsVolume::ParseBootSector() into real attribute/index parsing.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "looping-disk-reader.h"

using namespace NtfsBrowser;
using NtfsFuzz::LoopingDiskReader;

namespace
{

// NtfsBpb::signature (src/data/ntfs-bpb.h) sits 3 bytes in, after the boot
// sector's jump instruction - not at offset 0.
constexpr size_t kBpbSignatureOffset = 3;
constexpr char kBpbSignature[] = "NTFS    ";
constexpr size_t kBpbSignatureLen = 8;

void PatchBpbSignature(std::vector<BYTE>& data)
{
  if (data.size() >= kBpbSignatureOffset + kBpbSignatureLen)
  {
    std::memcpy(data.data() + kBpbSignatureOffset, kBpbSignature,
                kBpbSignatureLen);
  }
}

// Exercises the same path a real caller would: open the volume, parse the
// root file record, then walk its sub entries. Anything that throws here
// (bad_alloc, gsl::narrowing_error, ...) is a "handled" failure - genuine
// crashes (access violations, stack overflow, ...) are left to escape, which
// is exactly what AFL's forkserver/instrumentation detects via this
// process's exit status. No SEH: __try/__except doesn't exist under GCC, and
// AFL doesn't need it.
void FuzzOnce(std::vector<BYTE> data)
{
  PatchBpbSignature(data);

  NtfsVolume<Strategy::NO_CACHE> volume(
      std::make_unique<LoopingDiskReader>(std::move(data)));
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

}  // namespace

int main(int argc, char* argv[])
{
  if (argc != 2)
  {
    std::fprintf(stderr, "usage: %s <input-file>\n", argv[0]);
    return 1;
  }

  std::optional<std::vector<BYTE>> data =
      LoopingDiskReader::LoadFile(std::filesystem::path(argv[1]));
  if (!data)
  {
    // Empty/unreadable testcase: nothing a looping reader could serve.
    return 0;
  }

  try
  {
    FuzzOnce(std::move(*data));
  }
  catch (const std::exception&)
  {
  }
  catch (...)
  {
  }

  return 0;
}
