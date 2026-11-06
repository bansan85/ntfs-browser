#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <span>

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
#include "io/file-record-impl.h"
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

// What the checks on a record just read decided.
enum class HeaderVerdict : BYTE {
  Rejected,  // The record is unusable.
  Salvaged,  // Recovering: some sector fails its check, the others verify.
  Accepted   // The record is whole.
};

// Checks the magic of a record just read, and patches its update sequence.
// Strict rejects any sector that fails. Recovering patches the sectors that
// verify and salvages the record when at least one does: with none, nothing
// shows the bytes belong to this record. A record that does not carry the FILE
// magic (a BAAD one, or zeros) is never salvaged. fixup_failed is set when a
// sector failed its check, whatever the verdict.
template <Cache::Strategy S>
HeaderVerdict JudgeRecordHeader(Record::HeaderImpl<S>& header, bool recover,
                                bool& fixup_failed) {
  // Debug, not warning: a slot NTFS never used has no magic, so an MFT scan
  // meets this on every such slot. A caller gets Rejected either way.
  if (header.GetData()->magic != Data::FileRecordHeader::file_record_magic) {
    Log::Debug("Invalid file record");
    return HeaderVerdict::Rejected;
  }

  if (!recover) {
    if (!header.PatchUS()) {
      fixup_failed = true;
      Log::Warn("Update Sequence Number error");
      return HeaderVerdict::Rejected;
    }
    return HeaderVerdict::Accepted;
  }

  const size_t failed = header.PatchUSSalvage();
  if (failed == 0) {
    return HeaderVerdict::Accepted;
  }
  fixup_failed = true;
  if (failed >= header.SectorCount()) {
    Log::Warn("Update Sequence Number error: no sector of the record verifies");
    return HeaderVerdict::Rejected;
  }
  Log::Recoverable(
      true,
      "Update Sequence Number error: {} of {} sectors do not verify, the "
      "record is salvaged",
      failed, header.SectorCount());
  return HeaderVerdict::Salvaged;
}

}  // namespace

// Reads file record file_ref straight from disk, at a fixed offset from
// mft_addr, into record_buffer. False if the read fails.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ReadDirectRecord(ULONGLONG file_ref) {
  const std::optional<LONGLONG> addr = DirectRecordAddress(*volume, file_ref);
  if (!addr) {
    return false;
  }

  LARGE_INTEGER fr_addr{};
  fr_addr.QuadPart = *addr;
  const Bench::Detail::KindScope kind(Bench::ReadKind::Boot);
  return volume->ReadInto(fr_addr, record_buffer);
}

// Fills record_buffer with record file_ref, from the caller's record_source
// when it holds the bytes, else through $MFT's DATA attribute. $MFT may be
// fragmented, and its DATA attribute itself may be split across extension
// records: ReadData() picks whichever instance covers this offset. False if
// the read fails.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ReadRecordFromMft(ULONGLONG file_ref) {
  const std::optional<ULONGLONG> fr_addr =
      Mft::RecordOffset(volume->GetFileRecordSize(), file_ref);
  if (!fr_addr) {
    return false;
  }

  // The caller may hold the bytes the read would return.
  if (record_source) {
    const std::span<const BYTE> held = record_source(file_ref);
    if (held.size() == record_buffer.size()) {
      std::ranges::copy(held, record_buffer.begin());
      return true;
    }
  }

  const Bench::Detail::KindScope kind(Bench::ReadKind::Record, true);
  const std::optional<ULONGLONG> len =
      volume->ReadMftData(*fr_addr, record_buffer);
  return len && *len == volume->GetFileRecordSize();
}

// Parses the header of the record held in record_buffer. Null when the header
// constructor rejects it, as it does a forged record.
template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>> Io::FileRecord<S>::Impl::ParseHeader() {
  try {
    return std::make_unique<Record::HeaderImpl<S>>(record_buffer);
  } catch (const std::exception& e) {
    Log::Exception(e);
    return {};
  }
}

// Reads file record fileRef into record_buffer_ and returns its parsed
// header. Early records (and any record read before $MFT's own DATA
// attribute is known) come straight from disk at a fixed offset; later
// records go through $MFT's DATA attribute. A non-empty preread replaces the
// read.
template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>>
    Io::FileRecord<S>::Impl::ReadFileRecord(ULONGLONG file_ref,
                                            std::span<const BYTE> preread) {
  if (record_buffer.size() != volume->GetFileRecordSize()) {
    record_buffer.resize(volume->GetFileRecordSize());
  }

  bool filled = false;
  if (!preread.empty()) {
    // The header patches the update sequence in place, so it works on a copy.
    if (preread.size() != record_buffer.size()) {
      return {};
    }
    std::ranges::copy(preread, record_buffer.begin());
    filled = true;
  } else if (file_ref < static_cast<ULONGLONG>(Mft::Idx::User) ||
             !volume->HasMftBase()) {
    filled = ReadDirectRecord(file_ref);
  } else {
    filled = ReadRecordFromMft(file_ref);
  }
  if (!filled) {
    return {};
  }

  return ParseHeader();
}

// Reads the $MFTMirr copy of file_ref into record_buffer. Null when there is
// no copy to read, or it has no usable header.
template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>>
    Io::FileRecord<S>::Impl::ReadMirrorRecord(ULONGLONG file_ref) {
  if (!volume->ReadMftMirrorRecord(file_ref, record_buffer)) {
    return {};
  }

  return ParseHeader();
}

// The body of ParseFileRecord(), shared with callers that already hold the
// record's bytes.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ParseRecord(ULONGLONG file_ref,
                                          std::span<const BYTE> preread) {
  // Clear previous data
  ClearAttrs();
  if (file_record) {
    file_record.reset();
  }

  const bool recover = volume->GetOptions().recover_errors;
  failed_fixup = false;
  header_salvaged = false;
  reused_extensions.clear();
  overwritten_list = false;

  std::unique_ptr<Record::HeaderImpl<S>> header =
      ReadFileRecord(file_ref, preread);
  const bool read_ok = header != nullptr;
  if (!read_ok) {
    Log::Error("Cannot read file record {}", file_ref);
  }
  HeaderVerdict verdict =
      read_ok ? JudgeRecordHeader(*header, recover, failed_fixup)
              : HeaderVerdict::Rejected;

  // The mirror is the last resort: it may be older than $MFT's own record, as
  // NTFS refreshes it only now and then. The volume reads it only when
  // recovering, and only for the first four records. A caller that supplies
  // the bytes has no use for another source. It beats a salvaged record, whose
  // sectors are partly unverified.
  if (verdict != HeaderVerdict::Accepted && preread.empty()) {
    header.reset();
    header = ReadMirrorRecord(file_ref);
    bool mirror_fixup_failed = false;
    if (header && JudgeRecordHeader(*header, recover, mirror_fixup_failed) ==
                      HeaderVerdict::Accepted) {
      verdict = HeaderVerdict::Accepted;
      // The record of $MFT was damaged, and a copy stands in for it.
      header_salvaged = failed_fixup;
      Log::Info(
          "File record {} is unusable in $MFT: using its copy in $MFTMirr, "
          "which may be stale",
          file_ref);
    } else if (verdict == HeaderVerdict::Salvaged) {
      // The read of the mirror, if there was one, overwrote the record.
      header = ReadFileRecord(file_ref, preread);
      bool again_failed = false;
      verdict = header ? JudgeRecordHeader(*header, recover, again_failed)
                       : HeaderVerdict::Rejected;
    }
  }
  const bool accepted = verdict != HeaderVerdict::Rejected;
  header_salvaged = header_salvaged || verdict == HeaderVerdict::Salvaged;

  if (read_ok || accepted) {
    file_reference = file_ref;
  } else {
    file_reference = {};
  }
  if (!accepted) {
    return false;
  }

  Log::Debug("File Record {} Found", file_ref);
  file_record = std::move(header);

  return true;
}

template bool FileRecord<Cache::Strategy::NoCache>::Impl::ReadDirectRecord(
    ULONGLONG file_ref);
template bool FileRecord<Cache::Strategy::FullCache>::Impl::ReadDirectRecord(
    ULONGLONG file_ref);
template bool FileRecord<Cache::Strategy::NoCache>::Impl::ReadRecordFromMft(
    ULONGLONG file_ref);
template bool FileRecord<Cache::Strategy::FullCache>::Impl::ReadRecordFromMft(
    ULONGLONG file_ref);
template std::unique_ptr<Record::HeaderImpl<Cache::Strategy::NoCache>>
    FileRecord<Cache::Strategy::NoCache>::Impl::ParseHeader();
template std::unique_ptr<Record::HeaderImpl<Cache::Strategy::FullCache>>
    FileRecord<Cache::Strategy::FullCache>::Impl::ParseHeader();
template std::unique_ptr<Record::HeaderImpl<Cache::Strategy::NoCache>>
    FileRecord<Cache::Strategy::NoCache>::Impl::ReadFileRecord(
        ULONGLONG file_ref, std::span<const BYTE> preread);
template std::unique_ptr<Record::HeaderImpl<Cache::Strategy::FullCache>>
    FileRecord<Cache::Strategy::FullCache>::Impl::ReadFileRecord(
        ULONGLONG file_ref, std::span<const BYTE> preread);
template std::unique_ptr<Record::HeaderImpl<Cache::Strategy::NoCache>>
    FileRecord<Cache::Strategy::NoCache>::Impl::ReadMirrorRecord(
        ULONGLONG file_ref);
template std::unique_ptr<Record::HeaderImpl<Cache::Strategy::FullCache>>
    FileRecord<Cache::Strategy::FullCache>::Impl::ReadMirrorRecord(
        ULONGLONG file_ref);
template bool FileRecord<Cache::Strategy::NoCache>::Impl::ParseRecord(
    ULONGLONG file_ref, std::span<const BYTE> preread);
template bool FileRecord<Cache::Strategy::FullCache>::Impl::ParseRecord(
    ULONGLONG file_ref, std::span<const BYTE> preread);

}  // namespace NTFS_BROWSER_NAMESPACE::Io
