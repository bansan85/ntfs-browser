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

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/efs.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>  // IWYU pragma: keep
#include <ntfs-browser/log.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/mft-tree.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "gap-collation-probe.h"
#include "looping-disk-reader.h"
#include "named-stream-probe.h"

using NtfsBrowser::AttrBase;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::MftTree;
namespace Attr = NtfsBrowser::Attr;
namespace Efs = NtfsBrowser::Efs;
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

// Parses the root file record, then walks its sub entries.
//
// Templated on Strategy so the same input drives both NoCache and
// FullCache (see main()): some bugs only manifest in FullCache's object
// graph and are otherwise invisible to this fuzzer.
template <Cache::Strategy S>
void FuzzRootRecord(const NtfsVolume<S>& volume) {
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

// Stands in for a certificate store: the "wrapped" FEK the input stores is
// returned as if it were already decrypted. The input then picks the cipher
// and the key itself. A thumbprint that starts with refused_thumbprint models
// a key the provider does not hold.
class PassthroughKeyProvider final : public NtfsBrowser::Efs::IEfsKeyProvider {
 public:
  [[nodiscard]] std::optional<std::vector<BYTE>>
      UnwrapFek(std::span<const BYTE> thumbprint,
                std::span<const BYTE> wrapped_fek) const override {
    if (!thumbprint.empty() && thumbprint.front() == refused_thumbprint) {
      return std::nullopt;
    }
    return std::vector<BYTE>(wrapped_fek.begin(), wrapped_fek.end());
  }

  // First byte of a thumbprint this provider refuses to unwrap.
  static constexpr BYTE refused_thumbprint = 0xFF;
};

// How many bytes of each attribute the probe reads per call. Small enough to
// keep a huge declared data size from making the run long.
constexpr size_t probe_read_bytes = 8192;

// The metadata records parsed on top of the root: $MFT, $Volume, $Bitmap and
// $UpCase. The root has its own, narrower, parse in FuzzRootRecord().
constexpr std::array<Mft::Idx, 4> probed_records{
    Mft::Idx::Mft, Mft::Idx::Volume, Mft::Idx::Bitmap, Mft::Idx::UpCase};

// Reads the start, then the middle, then the end of one attribute's data.
template <Cache::Strategy S>
void ProbeAttrData(const AttrBase<S>& attr) {
  (void)attr.GetAttrType();
  (void)attr.GetAttrFlags();
  (void)attr.IsNonResident();
  (void)attr.IsUnNamed();
  (void)attr.GetAttrName();
  (void)attr.GetAllocatedSize();
  const ULONGLONG size = attr.GetDataSize();
  if (size == 0) {
    return;
  }

  std::vector<BYTE> buffer(probe_read_bytes);
  const std::span<BYTE> window(buffer);
  (void)attr.ReadData(0, window);
  (void)attr.ReadData(size / 2, window);
  // Past the end, where a read must come back short or empty.
  (void)attr.ReadData(size, window);
}

// Parses one record with every attribute type and calls the getters the
// directory walk never needs. Reads each attribute's data, which is where
// decompression and decryption happen.
template <Cache::Strategy S>
void ProbeRecord(const NtfsVolume<S>& volume, ULONGLONG record) {
  FileRecord file_record(volume);
  if (!file_record.ParseFileRecord(record) || !file_record.ParseAttrs()) {
    return;
  }

  (void)file_record.GetFileReference();
  (void)file_record.GetSequenceNumber();
  (void)file_record.GetBaseRecordReference();
  (void)file_record.IsExtensionRecord();
  (void)file_record.GetFileName();
  (void)file_record.GetFileSize();
  (void)file_record.GetAllocatedSize();
  FILETIME write_time{};
  FILETIME create_time{};
  FILETIME access_time{};
  FILETIME change_time{};
  file_record.GetFileTime(&write_time, &create_time, &access_time,
                          &change_time);
  (void)file_record.IsDeleted();
  (void)file_record.IsDirectory();
  (void)file_record.IsReadOnly();
  (void)file_record.IsHidden();
  (void)file_record.IsSystem();
  (void)file_record.IsArchive();
  (void)file_record.IsDevice();
  (void)file_record.IsNormal();
  (void)file_record.IsTemporary();
  (void)file_record.IsCompressed();
  (void)file_record.IsOffline();
  (void)file_record.IsNotContentIndexed();
  (void)file_record.IsEncrypted();
  (void)file_record.IsSparse();
  (void)file_record.IsReparsePoint();

  file_record.TraverseAttrs(
      [](const AttrBase<S>& attr, void*, bool*) { ProbeAttrData(attr); },
      nullptr);
}

// Rebuilds the namespace from the $MFT. The progress callback lets the scan
// run to its second call, a few thousand records in, and stops it there: a
// huge declared $MFT size cannot make the run long.
template <Cache::Strategy S>
void ProbeMftTree(const NtfsVolume<S>& volume) {
  const MftTree tree(volume,
                     {.progress = [](ULONGLONG done, ULONGLONG /*total*/) {
                       return done == 0;
                     }});
  (void)tree.Stats();
  (void)tree.Find(static_cast<ULONGLONG>(Mft::Idx::Root));
  (void)tree.Children(static_cast<ULONGLONG>(Mft::Idx::Root));
  for (const MftTree::Entry& entry : tree.Entries()) {
    (void)tree.IsReachable(entry.record);
    (void)tree.GetPath(entry.record);
    for (size_t i = 0; i < entry.names.size(); ++i) {
      (void)tree.GetPath(entry.record, i);
    }
  }
}

// Vetoes the attribute: the parse then skips it.
void DiscardAttr(const Attr::HeaderCommon& /*attr_head*/, bool& discard) {
  discard = true;
}

// Looks at the attribute and keeps it.
void KeepAttr(const Attr::HeaderCommon& /*attr_head*/, bool& discard) {
  discard = false;
}

// An attribute type past the last one the library knows.
constexpr auto unknown_attr_type = static_cast<Attr::Type>(0xFFFF0000);

// Calls the volume getters, then parses the root record with raw attribute
// callbacks installed on the volume and on the record.
template <Cache::Strategy S>
void ProbeVolumeApi(NtfsVolume<S>& volume) {
  (void)volume.GetOptions();
  (void)volume.GetVersion();
  (void)volume.GetRecordsCount();
  (void)volume.GetSectorSize();
  (void)volume.GetClusterSize();
  (void)volume.GetFileRecordSize();
  (void)volume.GetIndexBlockSize();
  (void)volume.GetMFTAddr();
  (void)volume.GetClusterBuffer();
  (void)volume.GetEfsKeyProvider();
  (void)volume.GetEfsCipherBackend();

  (void)volume.InstallAttrRawCB(unknown_attr_type, DiscardAttr);
  (void)volume.InstallAttrRawCB(Attr::Type::Data, DiscardAttr);
  (void)volume.InstallAttrRawCB(Attr::Type::IndexRoot, KeepAttr);
  FileRecord file_record(volume);
  (void)file_record.InstallAttrRawCB(unknown_attr_type, DiscardAttr);
  (void)file_record.InstallAttrRawCB(Attr::Type::StandardInformation,
                                     DiscardAttr);
  (void)file_record.InstallAttrRawCB(Attr::Type::FileName, KeepAttr);
  if (file_record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root))) {
    (void)file_record.ParseAttrs();
  }
  file_record.ClearAttrRawCB();
  volume.ClearAttrRawCB();
}

// Opens the volume, then runs every probe on it. A thrown exception counts
// as handled input rejection; only a real crash escapes, which AFL detects
// via this process's exit status.
//
// failingRead makes that one ReadInto() call fail, exercising the
// disk-read error paths a looping reader never reaches on its own.
template <Cache::Strategy S>
void FuzzOnce(std::span<const BYTE> data, const VolumeOptions& options,
              std::optional<size_t> failing_read = {}) {
  NtfsVolume<S> volume(std::make_unique<LoopingDiskReader>(data, failing_read),
                       options);
  if (!volume.IsVolumeOK()) {
    return;
  }

  // The real certificate store is never opened: the input is its own key.
  volume.SetEfsKeyProvider(std::make_shared<PassthroughKeyProvider>());

  FuzzRootRecord(volume);
  // One pass per cipher backend this build has: an unavailable one is
  // refused and skipped.
  for (const Efs::CipherBackend backend :
       {Efs::CipherBackend::CryptoPp, Efs::CipherBackend::BCrypt}) {
    if (volume.SetEfsCipherBackend(backend)) {
      ProbeRecord(volume, static_cast<ULONGLONG>(Mft::Idx::Root));
    }
  }
  for (const Mft::Idx record : probed_records) {
    ProbeRecord(volume, static_cast<ULONGLONG>(record));
  }
  ProbeMftTree(volume);
  ProbeVolumeApi(volume);
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
