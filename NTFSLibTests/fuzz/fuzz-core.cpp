#include "fuzz-core.h"

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/index-entry.h>  // IWYU pragma: keep
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "gap-collation-probe.h"
#include "looping-disk-reader.h"
#include "named-stream-probe.h"

using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::Attr::AttrBase;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Efs = NTFS_BROWSER_NAMESPACE::Efs;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace NtfsFuzz {

namespace {

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

// NtfsBpb::total_sectors sits at byte 40 of the boot sector.
constexpr size_t bpb_total_sectors_offset = 40;
// Passes any size check: the volume check compares it with a saturating
// byte count, so no real MFT address exceeds it.
constexpr std::uint64_t bpb_total_sectors =
    std::numeric_limits<std::uint64_t>::max();

// The BPB byte ranges a real NTFS volume keeps at zero: reserved_sectors up
// to the small sector count (bytes 14 to 20), sectors_per_fat (22 and 23) and
// the large sector count (32 to 35). The ranges leave out the media
// descriptor and not_used3, which are not checked.
struct ZeroRange {
  size_t offset;
  size_t size;
};

constexpr std::array<ZeroRange, 3> bpb_zero_ranges{{{14, 7}, {22, 2}, {32, 4}}};

// Overwrites the boot sector signature so ParseBootSector() accepts it. Also
// overwrites the 0xAA55 end marker, total_sectors and the zero ranges, each
// one when patch asks for it.
void PatchBpbSignature(std::span<BYTE> data, const BootSectorPatch& patch) {
  if (data.size() >= bpb_signature_offset + bpb_signature_len) {
    // The enclosing check leaves room for bpb_signature_len bytes at the
    // offset.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::memcpy(&data[bpb_signature_offset], bpb_signature.data(),
                bpb_signature_len);
  }
  if (patch.marker && data.size() >= bpb_marker_offset + bpb_marker.size()) {
    // The enclosing check leaves room for the marker at the offset.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::memcpy(&data[bpb_marker_offset], bpb_marker.data(), bpb_marker.size());
  }
  if (patch.total_sectors &&
      data.size() >= bpb_total_sectors_offset + sizeof(bpb_total_sectors)) {
    // The enclosing check leaves room for the field at the offset.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::memcpy(&data[bpb_total_sectors_offset], &bpb_total_sectors,
                sizeof(bpb_total_sectors));
  }
  if (patch.reserved_fields) {
    for (const ZeroRange& range : bpb_zero_ranges) {
      if (data.size() >= range.offset + range.size) {
        // The enclosing check leaves room for the range at the offset.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        std::memset(&data[range.offset], 0, range.size);
      }
    }
  }
}

// How many successive ReadInto() calls get a one-shot injected failure,
// one run each. Covers the boot sector, $MFT and root record reads and the
// first index block reads. Later reads mostly repeat those code paths.
constexpr size_t injected_failure_runs = 16;

// Ceiling of the eviction mode: one 64 KiB FullCache block, the smallest that
// still caches. Nearly every read then evicts the block before it.
constexpr std::uint64_t eviction_ceiling = 65536;

// The three VolumeOptions combinations every input is run under: strict (both
// flags off, the default), fully recovering (both on), and strict under a
// one-block cache ceiling. The first two exercise the "reject the damaged
// item whole" and "salvage it" code paths. The third replays the input with
// FullCache evicting, which only FullCache sees: NoCache ignores the ceiling.
constexpr std::array<VolumeOptions, 3> volume_option_modes{
    VolumeOptions{},
    VolumeOptions{.include_deleted = true, .recover_errors = true},
    VolumeOptions{.max_cache_bytes = eviction_ceiling}};

// Parses the root file record, then walks its sub entries.
//
// Templated on Strategy so the same input drives both NoCache and
// FullCache (see FuzzTestcase()): some bugs only manifest in FullCache's
// object graph and are otherwise invisible to this fuzzer.
template <Cache::Strategy S>
void FuzzRootRecord(const NtfsVolume<S>& volume) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord file_record(volume);
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
class PassthroughKeyProvider final
    : public NTFS_BROWSER_NAMESPACE::Efs::IEfsKeyProvider {
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
void ProbeAttrData(const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<S>& attr) {
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
  NTFS_BROWSER_NAMESPACE::Io::FileRecord file_record(volume);
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
      [](const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<S>& attr, void*, bool*) {
        ProbeAttrData(attr);
      },
      nullptr);
}

// Rebuilds the namespace from the $MFT. The progress callback lets the scan
// run to its second call, a few thousand records in, and stops it there: a
// huge declared $MFT size cannot make the run long.
template <Cache::Strategy S>
void ProbeMftTree(const NtfsVolume<S>& volume) {
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(
      volume, {.progress = [](ULONGLONG done, ULONGLONG /*total*/) {
        return done == 0;
      }});
  (void)tree.Stats();
  (void)tree.Find(static_cast<ULONGLONG>(Mft::Idx::Root));
  (void)tree.Children(static_cast<ULONGLONG>(Mft::Idx::Root));
  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry :
       tree.Entries()) {
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
  NTFS_BROWSER_NAMESPACE::Io::FileRecord file_record(volume);
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
  // refused and skipped. A build with none still parses the root once, so
  // the same parse paths run in every configuration.
  bool probed_root = false;
  for (const Efs::CipherBackend backend :
       {Efs::CipherBackend::CryptoPp, Efs::CipherBackend::BCrypt}) {
    if (volume.SetEfsCipherBackend(backend)) {
      ProbeRecord(volume, static_cast<ULONGLONG>(Mft::Idx::Root));
      probed_root = true;
    }
  }
  if (!probed_root) {
    ProbeRecord(volume, static_cast<ULONGLONG>(Mft::Idx::Root));
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

// Runs the input under both strategies. A mode that only changes the cache
// ceiling has nothing to say to NoCache, which ignores it, so it runs
// FullCache alone.
void RunBothStrategies(std::span<const BYTE> data, const VolumeOptions& options,
                       std::optional<size_t> failing_read = {}) {
  if (options.max_cache_bytes == VolumeOptions{}.max_cache_bytes) {
    RunGuarded<Cache::Strategy::NoCache>(data, options, failing_read);
  }
  RunGuarded<Cache::Strategy::FullCache>(data, options, failing_read);
}

}  // namespace

void FuzzTestcase(std::span<BYTE> data, bool inject_failures,
                  const BootSectorPatch& patch) {
  PatchBpbSignature(data, patch);

  // Guarded independently, so one run's exception can't skip the others.
  // Each strategy runs once per VolumeOptions mode, so the strict
  // (reject-whole), recovering (salvage) and evicting code paths are all
  // exercised.
  for (const VolumeOptions& options : volume_option_modes) {
    RunBothStrategies(data, options);
  }

  if (!inject_failures) {
    return;
  }

  for (size_t failing_read = 0; failing_read < injected_failure_runs;
       ++failing_read) {
    for (const VolumeOptions& options : volume_option_modes) {
      RunBothStrategies(data, options, failing_read);
    }
  }
}

namespace {

#ifdef _WIN32
constexpr std::wstring_view log_prefix = Log::option_prefix_w;
constexpr std::wstring_view inject_option = L"--inject-read-failures";
#else
constexpr std::string_view log_prefix = Log::option_prefix;
constexpr std::string_view inject_option = "--inject-read-failures";
#endif

// Prints command-line usage help.
void Usage(std::ostream& err, const std::basic_string<ArgChar>& program) {
  err << std::format(
      "usage: {} [--log=...] [--inject-read-failures] <input-file>\n",
      std::filesystem::path(program).string());
  err << std::format("  {}\n", Log::option_usage);
}

}  // namespace

int RunCommandLine(const std::vector<std::basic_string<ArgChar>>& args,
                   std::ostream& err) {
  const std::basic_string<ArgChar> program =
      args.empty() ? std::basic_string<ArgChar>() : args.front();
  // Trace on the console by default, so an afl-fuzz run and the saved
  // regression corpus both keep producing every message without a flag.
  Log::Config log_config{.console_level = Log::Level::Trace};
  const std::basic_string<ArgChar>* input = nullptr;
  bool inject_failures = false;

  for (const std::basic_string<ArgChar>& arg : args | std::views::drop(1)) {
    const std::basic_string_view<ArgChar> view(arg);
    if (view == inject_option) {
      inject_failures = true;
      continue;
    }
    if (view.starts_with(log_prefix)) {
      if (!Log::ParseOption(view, log_config)) {
        Usage(err, program);
        return 1;
      }
      continue;
    }
    if (input != nullptr) {
      Usage(err, program);
      return 1;
    }
    input = &arg;
  }

  if (input == nullptr) {
    Usage(err, program);
    return 1;
  }

  if (!Log::Configure(log_config)) {
    err << std::format("Cannot open log file {}\n",
                       log_config.file_path.string());
  }

  std::optional<std::vector<BYTE>> data =
      LoopingDiskReader::LoadFile(std::filesystem::path(*input));
  if (!data) {
    // Empty/unreadable testcase: nothing a looping reader could serve.
    return 0;
  }

  FuzzTestcase(*data, inject_failures);
  return 0;
}

}  // namespace NtfsFuzz
