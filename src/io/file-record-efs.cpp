#include "io/file-record-efs.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "attr/flags.h"
#include "attr/non-resident.h"  // IWYU pragma: keep
#include "efs/efs-context.h"
#include "efs/efs-stream.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// The largest $EFS stream read. It holds a few key entries, a few KiB at
// most. A forged size must not decide how much memory a parse allocates.
constexpr ULONGLONG max_efs_stream_size = 64ULL * 1024;

// Name of the $LOGGED_UTILITY_STREAM that holds the EFS keys.
constexpr std::wstring_view efs_stream_name = L"$EFS";

// Copies the key entries out of this record's $EFS stream. Returns none if the
// stream is absent or malformed: the parse goes on, and the read that needs
// the key fails, with the cause logged.
template <Cache::Strategy S>
std::vector<Efs::WrappedFek>
    ReadEfsEntries([[maybe_unused]] const FileRecord<S>& record) {
#if !(defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
      (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)))
  // Neither backend is compiled in: there is no decryptor to feed keys to.
  return {};
#else
  for (const std::unique_ptr<Attr::AttrBase<S>>& attr :
       record.GetAttr(Attr::Type::LoggedUtilityStream)) {
    if (attr->GetAttrName() != efs_stream_name) {
      continue;
    }

    const ULONGLONG size = attr->GetDataSize();
    if (size > max_efs_stream_size) {
      Log::Warn("$EFS stream is too large: {} bytes.", size);
      return {};
    }

    // Owned copy: under NoCache the attribute's bytes are short-lived.
    std::vector<BYTE> bytes(static_cast<size_t>(size));
    const std::optional<ULONGLONG> read = attr->ReadData(0, bytes);
    if (!read || *read != size) {
      Log::Warn("Cannot read the $EFS stream.");
      return {};
    }

    return Efs::ParseEfsStream(bytes).value_or(std::vector<Efs::WrappedFek>{});
  }

  return {};
#endif
}

}  // namespace

// Gives every encrypted $DATA stream of this record the context that
// decrypts it. Only a non-resident stream is encrypted: EFS never leaves file
// data inside the record. Returns false only when strict and one of the two
// anomalies below is found: the caller then rejects the whole record instead
// of reading the stream undecrypted.
template <Cache::Strategy S>
bool AttachEfsContext(FileRecord<S>& record) {
  const NtfsVolume<S>& volume = record.GetVolume();

  // Attr::HeaderCommon::flags bit 0: the on-disk "compressed" flag. Real NTFS
  // never sets it alongside 0x4000 (compression and encryption are mutually
  // exclusive), but a forged record could. Decrypting a compressed stream's
  // bytes before LZNT1 decoding sees them would corrupt them for no gain, so
  // that combination is left undecrypted rather than misprocessed, when
  // recovering. Checked with or without a decryption backend compiled in:
  // the anomaly is in the flags, not in what can decrypt them.
  const bool recover = volume.GetOptions().recover_errors;

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  std::vector<Attr::AttrNonResident<S>*> encrypted;
#endif

  for (const std::unique_ptr<Attr::AttrBase<S>>& attr :
       record.GetAttr(Attr::Type::Data)) {
    const WORD flags = attr->GetAttrFlags();
    if ((flags & Attr::flag_encrypted) == 0) {
      continue;
    }
    if ((flags & Attr::flag_compressed) != 0) {
      Log::Recoverable(recover,
                       "A $DATA stream is flagged both compressed and "
                       "encrypted; NTFS never combines them. Reading it "
                       "undecrypted.");
      if (!recover) {
        return false;
      }
      continue;
    }

    auto* non_resident = dynamic_cast<Attr::AttrNonResident<S>*>(attr.get());
    if (non_resident == nullptr) {
      Log::Recoverable(recover,
                       "A resident $DATA is flagged encrypted. Read as is.");
      if (!recover) {
        return false;
      }
      continue;
    }

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
    encrypted.push_back(non_resident);
#endif
  }

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  if (encrypted.empty()) {
    return true;
  }

  // One context for the record: its streams share one FEK, resolved once.
  const auto context = std::make_shared<const Efs::Context>(
      ReadEfsEntries(record), [&volume] { return volume.GetEfsKeyProvider(); },
      volume.GetEfsCipherBackend());
  for (Attr::AttrNonResident<S>* stream : encrypted) {
    stream->SetEfsContext(context);
  }
#endif
  return true;
}

template bool AttachEfsContext<Cache::Strategy::NoCache>(
    FileRecord<Cache::Strategy::NoCache>& record);
template bool AttachEfsContext<Cache::Strategy::FullCache>(
    FileRecord<Cache::Strategy::FullCache>& record);

}  // namespace NTFS_BROWSER_NAMESPACE::Io
