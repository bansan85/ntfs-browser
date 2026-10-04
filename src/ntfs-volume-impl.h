#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/efs.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "attr-slot.h"

#ifdef _WIN32
  #include <tchar.h>
#endif

namespace NtfsBrowser
{

// Caches reads from the volume's backing IDiskReader.
template <Strategy S>
class FileReader;

// The volume's $UpCase table.
class UpCaseTable;

// Everything NtfsVolume<S> keeps out of its public header: the members, and the
// private methods that work on them. FileRecord<S>, a friend of NtfsVolume<S>,
// reaches it through NtfsVolume<S>::impl_.
template <Strategy S>
class NtfsVolume<S>::Impl
{
 public:
  Impl(NtfsVolume<S>& self, const VolumeOptions& options);

  // The NtfsVolume this belongs to. The FileRecords it owns are built over it.
  NtfsVolume<S>* self_;
  ULONGLONG mft_addr_{0};
  std::unique_ptr<FileReader<S>> volume_;

  // MFT file records ($MFT file itself) may be fragmented
  // Get $MFT Data attribute to translate FileRecord to correct disk offset
  const AttrBase<S>* mft_data_{nullptr};  // $MFT Data Attribute (base extent)

  // The volume's own $UpCase, loaded on first use by GetUpCaseTable(). Stays
  // null when $UpCase cannot be read; upcase_loaded_ then keeps the failure
  // from being retried.
  mutable std::unique_ptr<const UpCaseTable> upcase_;

  FileRecord<S> mft_record_;  // $MFT File Record

  // EFS key source. efs_provider_set_ tells "never chosen", which lets the
  // default provider be created on the first decryption, from "chosen to be
  // none" (SetEfsKeyProvider(nullptr)), which disables decryption.
  mutable std::shared_ptr<Efs::IEfsKeyProvider> efs_provider_;

  // One VCN range $MFT's own DATA attribute maps: base extent or continuation.
  struct MftExtent
  {
    ULONGLONG start_vcn;
    ULONGLONG last_vcn;
    const AttrBase<S>* attr;
  };

  // Sorted by start_vcn; binary-searched per file-record read.
  std::vector<MftExtent> mft_extents_;

  // Owns extension FileRecords; std::list keeps FULL_CACHE pointers stable.
  std::list<FileRecord<S>> mft_extension_records_;

  mutable std::vector<BYTE> cluster_buffer_;

  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_{};
  DWORD cluster_size_{0};
  DWORD file_record_size_{0};
  DWORD index_block_size_{0};
  WORD sector_size_{0};
  bool volume_ok_{false};
  BYTE version_major_{0};
  BYTE version_minor_{0};
  mutable bool efs_provider_set_{false};

  // The selected EFS cipher backend. Defaults to whichever one is compiled
  // in, Crypto++ first.
#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
  Efs::CipherBackend efs_backend_{Efs::CipherBackend::kCryptoPp};
#elif defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
  Efs::CipherBackend efs_backend_{Efs::CipherBackend::kBCrypt};
#else
  Efs::CipherBackend efs_backend_{Efs::CipherBackend::kCryptoPp};
#endif
  mutable bool upcase_loaded_{false};

  // Fixed for the volume's lifetime; set by the constructor, read back
  // through GetOptions(). No setter: every component that reads it goes
  // through this one volume-wide copy.
  VolumeOptions options_;

#ifdef _WIN32
  [[nodiscard]] bool OpenVolume(_TCHAR volume);
  [[nodiscard]] bool OpenVolume(std::wstring_view path);
#endif
  [[nodiscard]] bool OpenVolume(std::unique_ptr<IDiskReader> reader);
  [[nodiscard]] bool ParseBootSector();
  void Init();
  void ResolveMftDataExtents();

  // One extension record $MFT's $ATTRIBUTE_LIST names for DATA, with the
  // sequence number its entries claim and the start VCN of each entry.
  struct PendingMftExtension
  {
    ULONGLONG record{0};
    WORD sequence{0};
    std::vector<ULONGLONG> start_vcns;
  };

  [[nodiscard]] static std::vector<PendingMftExtension>
      CollectPendingMftExtensions(const AttrBase<S>& rawList,
                                  ULONGLONG selfRef);
  void ResolvePendingMftExtension(const PendingMftExtension& item,
                                  ULONGLONG selfRef);
  void TryAddMftExtent(const AttrBase<S>& attr, ULONGLONG expectedStartVcn);
  [[nodiscard]] bool IsMftRangeMapped(ULONGLONG byteOffset,
                                      ULONGLONG length) const noexcept;
  [[nodiscard]] const MftExtent* FindMftExtent(ULONGLONG vcn) const noexcept;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadMftData(ULONGLONG offset, std::span<BYTE> buffer) const;
  [[nodiscard]] ULONGLONG GetRecordsCount() const noexcept;
  [[nodiscard]] const UpCaseTable& GetUpCaseTable() const;
  [[nodiscard]] std::unique_ptr<const UpCaseTable> LoadUpCaseTable() const;
  void AttrRawCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                       bool& bDiscard) const;
};

}  // namespace NtfsBrowser
