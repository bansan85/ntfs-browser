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

#include "gap-collation-probe.h"
#include "looping-disk-reader.h"
#include "named-stream-probe.h"

using namespace NtfsBrowser;
using NtfsFuzz::kGapCollationSearchName;
using NtfsFuzz::kNamedDataStreamName;
using NtfsFuzz::LoopingDiskReader;

namespace
{

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

// Opens the volume, parses the root file record, then walks its sub
// entries. A thrown exception counts as handled input rejection; only a
// real crash escapes, which AFL detects via this process's exit status.
//
// Templated on Strategy so the same input drives both NO_CACHE and
// FULL_CACHE (see main()): some bugs only manifest in FULL_CACHE's object
// graph and are otherwise invisible to this fuzzer.
template <Strategy S>
void FuzzOnce(const std::vector<BYTE>& data)
{
  // Copied so both strategies replay the exact same bytes independently.
  NtfsVolume<S> volume(std::make_unique<LoopingDiskReader>(data));
  if (!volume.IsVolumeOK())
  {
    return;
  }

  FileRecord fr(volume);
  // Without DATA here, FindStream() below never sees a named $DATA
  // attribute on ROOT to walk.
  fr.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION | Mask::DATA);
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

  // FindStream() calls GetAttrName() on every named $DATA attribute it
  // walks, regardless of the name passed in.
  (void)fr.FindStream(kNamedDataStreamName);

  // Unlike TraverseSubEntries() above, FindSubEntry() actually compares
  // names, exercising a real B+-tree sub-node descent.
  (void)fr.FindSubEntry(kGapCollationSearchName);
}

}

// Runs one AFL testcase file (argv[1]) through the library once.
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

  // Guarded independently, so one strategy's exception can't skip the other.
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
