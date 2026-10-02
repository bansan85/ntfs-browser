#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/index-entry.h>

#include "attr/filename.h"
#include "data/index-entry.h"
#include "flag/filename-namespace.h"
#include "flag/filename.h"

using NtfsBrowser::IndexEntry;

namespace
{

// Size of the buffer one fake index entry is built in, zero-filled.
constexpr size_t kEntryBufferSize = 256;

// The file reference the fake entries carry. Arbitrary.
constexpr DWORD kEntryRecordNumber = 42;

// UTF-16 code unit placed past the name, to catch a read beyond it.
constexpr WORD kFillerCodeUnit = 0xFFFF;

// Builds a raw $I30 index entry named "System" (file reference 42),
// followed in the same buffer by one filler UTF-16 code unit (0xFFFF)
// immediately past the name, so a read past the real name is detectable.
IndexEntry MakeSystemEntry()
{
  constexpr wchar_t kName[] = L"System";
  constexpr BYTE kNameLen = 6;

  auto buffer = std::shared_ptr<BYTE[]>(new BYTE[kEntryBufferSize]());

  auto& ie = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(buffer.get());
  ie.mft_index = kEntryRecordNumber;
  ie.mft_sn = 1;

  auto& fn = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&ie.stream);
  fn.flags = NtfsBrowser::Flag::Filename::DIRECTORY;
  fn.name_length = kNameLen;
  fn.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < kNameLen; i++)
  {
    fn.name[i] = gsl::narrow<WORD>(kName[i]);
  }
  // Filler: must never be read by Compare().
  fn.name[kNameLen] = kFillerCodeUnit;

  ie.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn.name[kNameLen]) -
                        reinterpret_cast<BYTE*>(&fn));
  ie.size = gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&ie.stream) -
                              reinterpret_cast<BYTE*>(&ie) + ie.stream_size);

  return IndexEntry(buffer, ie);
}

// Builds a single raw $I30 index entry with an arbitrary short name (used to
// probe individual code points' collation order).
IndexEntry MakeNamedEntry(std::wstring_view name)
{
  auto buffer = std::shared_ptr<BYTE[]>(new BYTE[kEntryBufferSize]());

  auto& ie = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(buffer.get());
  ie.mft_index = kEntryRecordNumber;
  ie.mft_sn = 1;

  auto& fn = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&ie.stream);
  fn.flags = NtfsBrowser::Flag::Filename::DIRECTORY;
  fn.name_length = gsl::narrow<BYTE>(name.size());
  fn.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (size_t i = 0; i < name.size(); i++)
  {
    fn.name[i] = gsl::narrow<WORD>(name[i]);
  }

  ie.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn.name[name.size()]) -
                        reinterpret_cast<BYTE*>(&fn));
  ie.size = gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&ie.stream) -
                              reinterpret_cast<BYTE*>(&ie) + ie.stream_size);

  return IndexEntry(buffer, ie);
}

}  // namespace

TEST_CASE("Compare orders code points in the Z-a gap by uppercase collation",
          "[filename][regression]")
{
  const IndexEntry entry = MakeNamedEntry(L"a");
  REQUIRE(entry.HasName());
  REQUIRE(entry.GetFilename() == L"a");

  // NTFS-correct: '_' (0x5F) sorts after 'a' (folds to 'A' = 0x41).
  CHECK(entry.Compare(L"_") > 0);
}

TEST_CASE("Compare treats a name as a prefix, not extended by trailing bytes",
          "[filename][regression]")
{
  const IndexEntry entry = MakeSystemEntry();
  REQUIRE(entry.HasName());
  REQUIRE(entry.GetFilename() == L"System");

  CHECK(entry.Compare(L"System32") > 0);
  CHECK(entry.Compare(L"System") == 0);
}

TEST_CASE("Compare folds non-ASCII case without depending on the C locale",
          "[filename][upcase][regression]")
{
  const IndexEntry entry = MakeNamedEntry(L"\u00E9");
  REQUIRE(entry.HasName());

  CHECK(entry.Compare(L"\u00C9") == 0);
  CHECK(entry.Compare(L"\u00D6") > 0);
  CHECK(entry.Compare(L"\u00E9") == 0);
}
