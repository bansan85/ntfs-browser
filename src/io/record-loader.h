#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

#include "record/header.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Io {

// Brings one file record into a buffer, checks its magic, patches its update
// sequence and returns its header. Reads the record from the caller's
// RecordSource, from $MFT, or straight from disk while $MFT is not known yet.
// When recovering, a record that $MFT cannot give is looked up in $MFTMirr.
// Short-lived: built over the buffer of a FileRecord for one Load().
template <Cache::Strategy S>
class RecordLoader {
 public:
  using RecordSource = typename FileRecord<S>::RecordSource;

  // What Load() decided about the record.
  struct Loaded {
    // The header of the accepted record. Null when it was rejected.
    std::unique_ptr<Record::HeaderImpl<S>> header;
    // The bytes were read and had a header, whatever the checks then said.
    bool read_ok{false};
    // The record is usable: whole, salvaged, or taken from $MFTMirr.
    bool accepted{false};
    // The update sequence check of the record failed on at least one sector.
    bool failed_fixup{false};
    // Recovering kept a record whose update sequence check failed, or read it
    // from $MFTMirr instead.
    bool salvaged{false};
  };

  // The volume, the buffer and the source MUST outlive this object. buffer
  // holds the bytes of the record after Load(), and the header views them.
  // Whether the load recovers comes from the options of the volume.
  RecordLoader(const NtfsVolume<S>& volume, std::vector<BYTE>& buffer,
               const RecordSource& source);

  // Reads record file_ref into the buffer and judges it. A non-empty preread
  // holds the bytes of the record already: they are copied and no other source
  // is tried.
  [[nodiscard]] Loaded Load(ULONGLONG file_ref, std::span<const BYTE> preread);

 private:
  // What the checks on a record just read decided.
  enum class Verdict : BYTE {
    Rejected,  // The record is unusable.
    Salvaged,  // Recovering: some sector fails its check, the others verify.
    Accepted   // The record is whole.
  };

  // Checks the magic of a record just read, and patches its update sequence.
  // Strict rejects any sector that fails. Recovering patches the sectors that
  // verify and salvages the record when at least one does: with none, nothing
  // shows the bytes belong to this record. A record that does not carry the
  // FILE magic (a BAAD one, or zeros) is never salvaged. fixup_failed is set
  // when a sector failed its check, whatever the verdict.
  [[nodiscard]] Verdict Judge(Record::HeaderImpl<S>& header,
                              bool& fixup_failed) const;

  // The last resort for a record that is not whole: its $MFTMirr copy, which
  // may be older than the record of $MFT, as NTFS refreshes it only now and
  // then. The volume reads it only when recovering, and only for the first
  // four records. It beats a salvaged record, whose sectors are partly
  // unverified. When it fails and the record was salvaged, the record is read
  // again, as the read of the copy overwrote the buffer. Returns the new
  // verdict, and sets header to the one that goes with it.
  [[nodiscard]] Verdict
      TryMirror(ULONGLONG file_ref, Verdict verdict,
                std::unique_ptr<Record::HeaderImpl<S>>& header, Loaded& loaded);

  // Reads file_ref straight from disk, at a fixed offset from the start of
  // $MFT. False if the read fails.
  [[nodiscard]] bool ReadDirect(ULONGLONG file_ref);

  // Fills the buffer from the RecordSource when it holds the bytes, else
  // through $MFT's DATA attribute, which may be fragmented and split across
  // extension records. False if the read fails.
  [[nodiscard]] bool ReadFromMft(ULONGLONG file_ref);

  // The header of the buffer. Null when the header constructor rejects it, as
  // it does a forged record.
  [[nodiscard]] std::unique_ptr<Record::HeaderImpl<S>> ParseHeader();

  // Reads file_ref and returns its header. Early records, and any record read
  // before $MFT's own DATA attribute is known, come straight from disk. The
  // others go through $MFT's DATA attribute.
  [[nodiscard]] std::unique_ptr<Record::HeaderImpl<S>>
      ReadRecord(ULONGLONG file_ref, std::span<const BYTE> preread);

  // Reads the $MFTMirr copy of file_ref into the buffer. Null when there is no
  // copy to read, or it has no usable header.
  [[nodiscard]] std::unique_ptr<Record::HeaderImpl<S>>
      ReadMirror(ULONGLONG file_ref);

  const NtfsVolume<S>& volume_;
  std::vector<BYTE>& buffer_;
  const RecordSource& source_;
  bool recover_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
