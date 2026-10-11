#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/index-entry.h>

#include "data/filename-flag.h"
#include "data/filename-namespace.h"
#include "data/filename.h"
#include "data/index-entry.h"

using NtfsBrowser::IndexEntry;
using NtfsBrowser::IndexEntryView;

namespace {

// Size of the buffer one fake index entry is built in, zero-filled.
constexpr size_t entry_buffer_size = 256;

// The file reference the fake entries carry. Arbitrary.
constexpr DWORD entry_record_number = 42;

// UTF-16 code unit placed past the name, to catch a read beyond it.
constexpr WORD filler_code_unit = 0xFFFF;

// Builds a raw $I30 index entry named "System" (file reference 42),
// followed in the same buffer by one filler UTF-16 code unit (0xFFFF)
// immediately past the name, so a read past the real name is detectable.
IndexEntry MakeSystemEntry() {
  constexpr std::wstring_view name_value = L"System";
  constexpr BYTE name_len = 6;

  std::vector<BYTE> buffer(entry_buffer_size);

  auto& index_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(buffer.data());
  index_entry.mft_index = entry_record_number;
  index_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&index_entry.stream);
  filename.flags = NtfsBrowser::Data::FilenameFlag::Directory;
  filename.name_length = name_len;
  filename.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
  for (BYTE i = 0; i < name_len; i++) {
    // i is below name_len, the length of name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name_value[i]);
  }
  // Filler: must never be read by Compare().
  filename.name[name_len] = filler_code_unit;

  index_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[name_len]) -
                        reinterpret_cast<BYTE*>(&filename));
  index_entry.size = gsl::narrow<WORD>(&index_entry.stream -
                                       reinterpret_cast<BYTE*>(&index_entry) +
                                       index_entry.stream_size);

  return IndexEntry(IndexEntryView(index_entry));
}

// Builds a single raw $I30 index entry with an arbitrary short name (used to
// probe individual code points' collation order).
IndexEntry MakeNamedEntry(std::wstring_view name) {
  std::vector<BYTE> buffer(entry_buffer_size);

  auto& index_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(buffer.data());
  index_entry.mft_index = entry_record_number;
  index_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&index_entry.stream);
  filename.flags = NtfsBrowser::Data::FilenameFlag::Directory;
  filename.name_length = gsl::narrow<BYTE>(name.size());
  filename.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
  for (size_t i = 0; i < name.size(); i++) {
    // i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name[i]);
  }

  index_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[name.size()]) -
                        reinterpret_cast<BYTE*>(&filename));
  index_entry.size = gsl::narrow<WORD>(&index_entry.stream -
                                       reinterpret_cast<BYTE*>(&index_entry) +
                                       index_entry.stream_size);

  return IndexEntry(IndexEntryView(index_entry));
}

}  // namespace

TEST_CASE("Compare orders code points in the Z-a gap by uppercase collation",
          "[filename][regression]") {
  const IndexEntry entry = MakeNamedEntry(L"a");
  REQUIRE(entry.HasName());
  REQUIRE(entry.GetFilename() == L"a");

  // NTFS-correct: '_' (0x5F) sorts after 'a' (folds to 'A' = 0x41).
  CHECK(entry.Compare(L"_") > 0);
}

TEST_CASE("Compare treats a name as a prefix, not extended by trailing bytes",
          "[filename][regression]") {
  const IndexEntry entry = MakeSystemEntry();
  REQUIRE(entry.HasName());
  REQUIRE(entry.GetFilename() == L"System");

  CHECK(entry.Compare(L"System32") > 0);
  CHECK(entry.Compare(L"System") == 0);
}

TEST_CASE("A copied IndexEntry reads its own bytes, not the original's",
          "[filename][regression]") {
  std::optional<IndexEntry> original = MakeSystemEntry();
  const IndexEntry copy(*original);
  original.reset();

  REQUIRE(copy.HasName());
  CHECK(copy.GetFilename() == L"System");
  CHECK(copy.GetFileReference() == entry_record_number);
  CHECK(copy.IsDirectory());
}

TEST_CASE("Compare folds non-ASCII case without depending on the C locale",
          "[filename][upcase][regression]") {
  const IndexEntry entry = MakeNamedEntry(L"\u00E9");
  REQUIRE(entry.HasName());

  CHECK(entry.Compare(L"\u00C9") == 0);
  CHECK(entry.Compare(L"\u00D6") > 0);
  CHECK(entry.Compare(L"\u00E9") == 0);
}
