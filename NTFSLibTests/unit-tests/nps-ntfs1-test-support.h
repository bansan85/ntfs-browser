#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/strategy.h>

#include "corpus-test-support.h"

namespace NtfsBrowser
{
template <Strategy S>
class FileRecord;
template <Strategy S>
class NtfsVolume;
}  // namespace NtfsBrowser

namespace NtfsBrowserTests
{

// The NPS Test Disk Image "nps-2009-ntfs1" (Digital Corpora), generation 2: a
// real NTFS volume with RAW, Compressed, and Encrypted directories, each
// holding the same five files, plus the EFS recovery keys at the root.
// Converted from the corpus's published .E01 to raw with ewfexport (see
// narrative.txt and ntfs1-gen2.xml alongside the source images).
inline const std::filesystem::path ntfs1_image =
    NpsNtfs1Dir() / "ntfs1-gen2.raw";

// One of the five files RAW/, Compressed/, and Encrypted/ all hold. size and
// md5 are the plaintext's, from ntfs1-gen2.xml (the fiwalk ground-truth
// report published alongside this corpus). Compressed's own on-disk hash
// already matches this (fiwalk decompresses NTFS compression when hashing);
// Encrypted's does not, since fiwalk never decrypted EFS - this is the
// RAW/decrypted hash in both cases.
struct KnownFile
{
  std::string_view name;
  ULONGLONG size;
  std::string_view md5;
};

inline constexpr std::array<KnownFile, 5> known_files{{
    {"20076517123273.pdf", 1001647, "2e167810afd3e5398b3fc439a99a4ef2"},
    {"NISTSP800-88_rev1.pdf", 554121, "e9103614acfb2cdb5d1ac52e387bf2a3"},
    {"NIST_logo.jpg", 2205, "d651f3132416847bfe874d3d63a4198a"},
    {"report02-3.pdf", 1421998, "dede94f84fb2d00dc93ed00fda272a18"},
    // Written one line at a time, interleaved with its two copies in the
    // other directories: naturally, heavily fragmented.
    {"logfile1.txt", 21888890, "be2828dda150f19edf9a0fc87e3ab640"},
}};

// Opens ntfs1_image through a PartitionDiskReader, so NtfsVolume's own
// path-based constructor - Windows-only, since it goes through
// Win32DiskReader - is never needed for this corpus's bare-volume image.
[[nodiscard]] std::unique_ptr<NtfsBrowser::IDiskReader> OpenNtfs1Image();

// Parses dir's own file record as the volume's root directory, ready for
// FindSubEntry(). dir must already be constructed on that volume.
void OpenRootDir(NtfsBrowser::FileRecord<NtfsBrowser::Strategy::NoCache>& dir);

// Looks up name under dir's current directory and reparses dir in place as
// that subdirectory.
void OpenSubDir(NtfsBrowser::FileRecord<NtfsBrowser::Strategy::NoCache>& dir,
                std::string_view name);

// Looks up name under dir and parses file in place as it, ready for
// FindStream(). file's storage is the caller's: it bounds the lifetime of
// any attribute pointer FindStream() later returns on it.
void OpenFile(
    NtfsBrowser::FileRecord<NtfsBrowser::Strategy::NoCache>& file,
    const NtfsBrowser::FileRecord<NtfsBrowser::Strategy::NoCache>& dir,
    std::string_view name);

// Reads name's whole unnamed $DATA stream out of dir.
[[nodiscard]] std::vector<BYTE> ReadFile(
    const NtfsBrowser::NtfsVolume<NtfsBrowser::Strategy::NoCache>& volume,
    const NtfsBrowser::FileRecord<NtfsBrowser::Strategy::NoCache>& dir,
    std::string_view name);

}  // namespace NtfsBrowserTests
