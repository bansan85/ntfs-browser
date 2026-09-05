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
//
// Templated on Strategy so the exact same input drives both NO_CACHE and
// FULL_CACHE (see the two calls in main()) - some bugs (eg. F19, N3, see
// docs/bug-reports/2026-09-03-full-repo.md) only manifest in FULL_CACHE's
// object graph (by-value record buffers, cached 64KiB blocks) and are
// otherwise entirely invisible to this fuzzer, which used to hardcode
// NO_CACHE only.
template <Strategy S>
void FuzzOnce(const std::vector<BYTE>& data)
{
  // Each strategy gets its own LoopingDiskReader over its own copy of data,
  // so both runs replay the exact same bytes from the same starting
  // position, independently of each other.
  NtfsVolume<S> volume(std::make_unique<LoopingDiskReader>(data));
  if (!volume.IsVolumeOK())
  {
    return;
  }

  FileRecord fr(volume);
  // DATA is included alongside INDEX_ROOT/INDEX_ALLOCATION so the
  // FindStream() call below actually has named $DATA attributes (if any
  // are present on ROOT) to look through - see F2 in
  // docs/bug-reports/2026-09-03-full-repo.md.
  fr.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION | Mask::DATA);
  if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Enum::MftIdx::ROOT)))
  {
    // Exercises the F15 defensive check (src/file-record.cpp): file_record_
    // is guaranteed empty here (the constructor default-initializes it, and
    // a failed ParseFileRecord() never assigns it), so IsDeleted()/
    // IsDirectory() must return false - and trace, not crash, which was UB
    // pre-fix on an empty file_record_ optional - rather than reaching the
    // normal path below, which requires a successful parse. See F15 in
    // docs/bug-reports/2026-09-03-full-repo.md. Results discarded, only
    // reaching the trace calls matters here.
    (void)fr.IsDeleted();
    (void)fr.IsDirectory();
    return;
  }
  if (!fr.ParseAttrs())
  {
    return;
  }

  fr.TraverseSubEntries([](const IndexEntry&, void*) {}, nullptr);

  // Exercises FindStream() with a non-empty name, unlike this repo's only
  // two real callers (both pass {}): FindStream() calls GetAttrName() on
  // every named $DATA attribute it walks regardless of what name it was
  // asked to find, so this alone is enough to reach the F2 fix's bound
  // check. The result is intentionally discarded - only reaching
  // GetAttrName() matters here, not whether a stream by this name exists.
  const AttrBase<S>* stream = fr.FindStream(L"F2-probe");
  (void)stream;
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

  PatchBpbSignature(*data);

  // Run both strategies against the same input, each independently
  // try/catch-guarded so a "handled" exception (or crash) in one doesn't
  // prevent the other from running.
  try
  {
    FuzzOnce<Strategy::NO_CACHE>(*data);
  }
  catch (const std::exception&)
  {
  }
  catch (...)
  {
  }

  try
  {
    FuzzOnce<Strategy::FULL_CACHE>(*data);
  }
  catch (const std::exception&)
  {
  }
  catch (...)
  {
  }

  return 0;
}
