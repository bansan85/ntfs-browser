#include "io/record-loader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "bench/recorder.h"
#include "data/file-record-header.h"
#include "log/ntfs-common.h"
#include "mft/record-offset.h"
#include "record/header.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Byte offset of record file_ref when $MFT is taken as one continuous disk
// run. Empty when the offset does not fit a LONGLONG.
template <Cache::Strategy S>
std::optional<LONGLONG> DirectRecordAddress(const NtfsVolume<S>& volume,
                                            ULONGLONG file_ref) {
  const std::optional<ULONGLONG> offset =
      Mft::RecordOffset(volume.GetFileRecordSize(), file_ref);
  if (!offset) {
    return std::nullopt;
  }

  try {
    return gsl::narrow<LONGLONG>(volume.GetMFTAddr() + *offset);
  } catch (const std::exception& e) {
    // fileRef is attacker-controlled and unbounded, so this sum can
    // still overflow a LONGLONG even with mft_addr_ validated.
    Log::Exception(e);
    return std::nullopt;
  }
}

}  // namespace

template <Cache::Strategy S>
RecordLoader<S>::RecordLoader(const NtfsVolume<S>& volume,
                              std::vector<BYTE>& buffer,
                              const RecordSource& source)
    : volume_(volume),
      buffer_(buffer),
      source_(source),
      recover_(volume.GetOptions().recover_errors) {}

template <Cache::Strategy S>
typename RecordLoader<S>::Verdict
    RecordLoader<S>::Judge(Record::HeaderImpl<S>& header,
                           bool& fixup_failed) const {
  // Debug, not warning: a slot NTFS never used has no magic, so an MFT scan
  // meets this on every such slot. A caller gets Rejected either way.
  if (header.GetData()->magic != Data::FileRecordHeader::file_record_magic) {
    Log::Debug("Invalid file record");
    return Verdict::Rejected;
  }

  if (!recover_) {
    if (!header.PatchUS()) {
      fixup_failed = true;
      Log::Warn("Update Sequence Number error");
      return Verdict::Rejected;
    }
    return Verdict::Accepted;
  }

  const size_t failed = header.PatchUSSalvage();
  if (failed == 0) {
    return Verdict::Accepted;
  }
  fixup_failed = true;
  if (failed >= header.SectorCount()) {
    Log::Warn("Update Sequence Number error: no sector of the record verifies");
    return Verdict::Rejected;
  }
  Log::Recoverable(
      true,
      "Update Sequence Number error: {} of {} sectors do not verify, the "
      "record is salvaged",
      failed, header.SectorCount());
  return Verdict::Salvaged;
}

template <Cache::Strategy S>
bool RecordLoader<S>::ReadDirect(ULONGLONG file_ref) {
  const std::optional<LONGLONG> addr = DirectRecordAddress(volume_, file_ref);
  if (!addr) {
    return false;
  }

  LARGE_INTEGER fr_addr{};
  fr_addr.QuadPart = *addr;
  const Bench::Detail::KindScope kind(Bench::ReadKind::Boot);
  return volume_.ReadInto(fr_addr, buffer_);
}

template <Cache::Strategy S>
bool RecordLoader<S>::ReadFromMft(ULONGLONG file_ref) {
  const std::optional<ULONGLONG> fr_addr =
      Mft::RecordOffset(volume_.GetFileRecordSize(), file_ref);
  if (!fr_addr) {
    return false;
  }

  // The caller may hold the bytes the read would return.
  if (source_) {
    const std::span<const BYTE> held = source_(file_ref);
    if (held.size() == buffer_.size()) {
      std::ranges::copy(held, buffer_.begin());
      return true;
    }
  }

  const Bench::Detail::KindScope kind(Bench::ReadKind::Record, true);
  const std::optional<ULONGLONG> len = volume_.ReadMftData(*fr_addr, buffer_);
  return len && *len == volume_.GetFileRecordSize();
}

template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>> RecordLoader<S>::ParseHeader() {
  try {
    return std::make_unique<Record::HeaderImpl<S>>(buffer_);
  } catch (const std::exception& e) {
    Log::Exception(e);
    return {};
  }
}

template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>>
    RecordLoader<S>::ReadRecord(ULONGLONG file_ref,
                                std::span<const BYTE> preread) {
  if (buffer_.size() != volume_.GetFileRecordSize()) {
    buffer_.resize(volume_.GetFileRecordSize());
  }

  bool filled = false;
  if (!preread.empty()) {
    // The header patches the update sequence in place, so it works on a copy.
    if (preread.size() != buffer_.size()) {
      return {};
    }
    std::ranges::copy(preread, buffer_.begin());
    filled = true;
  } else if (file_ref < static_cast<ULONGLONG>(Mft::Idx::User) ||
             !volume_.HasMftBase()) {
    filled = ReadDirect(file_ref);
  } else {
    filled = ReadFromMft(file_ref);
  }
  if (!filled) {
    return {};
  }

  return ParseHeader();
}

template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>>
    RecordLoader<S>::ReadMirror(ULONGLONG file_ref) {
  if (!volume_.ReadMftMirrorRecord(file_ref, buffer_)) {
    return {};
  }

  return ParseHeader();
}

template <Cache::Strategy S>
typename RecordLoader<S>::Verdict
    RecordLoader<S>::TryMirror(ULONGLONG file_ref, Verdict verdict,
                               std::unique_ptr<Record::HeaderImpl<S>>& header,
                               Loaded& loaded) {
  header.reset();
  header = ReadMirror(file_ref);
  bool mirror_fixup_failed = false;
  if (header && Judge(*header, mirror_fixup_failed) == Verdict::Accepted) {
    // The record of $MFT was damaged, and a copy stands in for it.
    loaded.salvaged = loaded.failed_fixup;
    Log::Info(
        "File record {} is unusable in $MFT: using its copy in $MFTMirr, "
        "which may be stale",
        file_ref);
    return Verdict::Accepted;
  }
  if (verdict != Verdict::Salvaged) {
    return verdict;
  }
  header = ReadRecord(file_ref, {});
  bool again_failed = false;
  return header ? Judge(*header, again_failed) : Verdict::Rejected;
}

template <Cache::Strategy S>
typename RecordLoader<S>::Loaded
    RecordLoader<S>::Load(ULONGLONG file_ref, std::span<const BYTE> preread) {
  Loaded loaded;
  std::unique_ptr<Record::HeaderImpl<S>> header = ReadRecord(file_ref, preread);
  loaded.read_ok = header != nullptr;
  if (!loaded.read_ok) {
    Log::Error("Cannot read file record {}", file_ref);
  }
  Verdict verdict =
      loaded.read_ok ? Judge(*header, loaded.failed_fixup) : Verdict::Rejected;

  // A caller that supplies the bytes has no use for another source.
  if (verdict != Verdict::Accepted && preread.empty()) {
    verdict = TryMirror(file_ref, verdict, header, loaded);
  }
  loaded.accepted = verdict != Verdict::Rejected;
  loaded.salvaged = loaded.salvaged || verdict == Verdict::Salvaged;
  if (!loaded.accepted) {
    return loaded;
  }

  Log::Debug("File Record {} Found", file_ref);
  loaded.header = std::move(header);
  return loaded;
}

template class RecordLoader<Cache::Strategy::NoCache>;
template class RecordLoader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
