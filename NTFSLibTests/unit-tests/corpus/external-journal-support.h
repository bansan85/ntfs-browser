#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>

#include "external-journal-samples.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

struct VolumeOptions;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Tests::RealJournal {

// Which kept project holds a fixture.
enum class Project : unsigned char { DfirNtfs, DfirMuseum };

// One kept real $J, with what an independent reader found in it.
struct Fixture {
  std::string_view label;
  Project project;
  // Path under the project's folder.
  std::string_view relative_path;
  std::uintmax_t file_size;
  // V2 and V3 records, the ones that carry a name.
  std::size_t record_count;
  // V4 records: well-formed, and without a name.
  std::size_t range_record_count;
  // USN of the first record. The file starts there: NTFS cut the sparse head
  // off, and the fixture keeps none of it.
  std::uint64_t base_usn;
  std::span<const JournalSample, sample_count> samples;
};

// The five kept journals: Server 2012 R2, Windows 10, Windows 11, Server 2022
// and the dfir_ntfs one.
[[nodiscard]] const std::array<Fixture, 5>& Fixtures();

// The fixture with this label. Fails the running test when there is none.
[[nodiscard]] const Fixture& FixtureNamed(std::string_view label);

// Path of fixture on this machine. SKIPs the running test when the file is
// absent, or fails it under NTFS_BROWSER_REQUIRE_TEST_DATA.
[[nodiscard]] std::filesystem::path RequirePathOf(const Fixture& fixture);

// Serves stream as $UsnJrnl:$J of a fake volume, after sparse_head_bytes of
// hole, and reads its journal with options. Empty when the volume gives none,
// which is what strict mode does. Fails the running test when the volume does
// not open. sparse_head_bytes MUST be a multiple of the fake cluster size.
[[nodiscard]] std::optional<Usn::Journal>
    ReadJournalOf(std::span<const BYTE> stream, ULONGLONG sparse_head_bytes,
                  Cache::Strategy strategy, const VolumeOptions& options);

// A record of the text `fsutil usn readjournal` prints.
struct FsutilRecord {
  LONGLONG usn{0};
  // Empty for a V4 record, which carries no name.
  std::string name;
  bool has_name{false};
  // Value of "File name length", in bytes.
  std::uint64_t name_length{0};
  DWORD reason{0};
  // Value of "Time stamp", as whole seconds since 1601-01-01. fsutil prints
  // the time of the machine's time zone: the caller undoes it.
  std::uint64_t seconds{0};
  DWORD attributes{0};
  // The two halves of the 128-bit "File ID".
  ULONGLONG file_id_low{0};
  ULONGLONG file_id_high{0};
  ULONGLONG parent_id_low{0};
  ULONGLONG parent_id_high{0};
  // What fsutil reports: the version it converted the record to.
  unsigned major_version{0};
};

// What `fsutil usn readjournal` printed for a whole journal.
struct FsutilListing {
  // "Next USN" of the header: where the journal ended when fsutil ran.
  ULONGLONG next_usn{0};
  std::vector<FsutilRecord> records;
};

// Parses an fsutil listing. LF and CRLF line ends are both accepted. A line
// without its colon, or a value that does not parse, fails the running test.
[[nodiscard]] FsutilListing
    ParseFsutilListing(const std::filesystem::path& file);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::RealJournal
