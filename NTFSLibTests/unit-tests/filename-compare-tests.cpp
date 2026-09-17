#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/index-entry.h>

#include "attr/filename.h"
#include "data/index-entry.h"
#include "flag/filename-namespace.h"
#include "flag/filename.h"
#include "flag/index-entry.h"

using NtfsBrowser::IndexEntry;

namespace
{

// Builds a raw $I30 index entry named "System" (file reference 42),
// followed in the same buffer by one filler UTF-16 code unit (0xFFFF)
// immediately past the name, so a read past the real name is detectable.
IndexEntry MakeSystemEntry()
{
  constexpr wchar_t kName[] = L"System";
  constexpr BYTE kNameLen = 6;

  auto buffer = std::shared_ptr<BYTE[]>(new BYTE[256]());

  auto& ie = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(buffer.get());
  ie.mft_index = 42;
  ie.mft_sn = 1;

  auto& fn = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&ie.stream);
  fn.flags = NtfsBrowser::Flag::Filename::DIRECTORY;
  fn.name_length = kNameLen;
  fn.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < kNameLen; i++)
  {
    fn.name[i] = static_cast<WORD>(kName[i]);
  }
  // Filler: must never be read by Compare().
  fn.name[kNameLen] = 0xFFFF;

  ie.stream_size =
      static_cast<WORD>(reinterpret_cast<BYTE*>(&fn.name[kNameLen]) -
                        reinterpret_cast<BYTE*>(&fn));
  ie.size = static_cast<WORD>(reinterpret_cast<BYTE*>(&ie.stream) -
                              reinterpret_cast<BYTE*>(&ie) + ie.stream_size);

  return IndexEntry(buffer, ie);
}

// Builds a single raw $I30 index entry with an arbitrary short name (used to
// probe individual code points' collation order).
IndexEntry MakeNamedEntry(std::wstring_view name)
{
  auto buffer = std::shared_ptr<BYTE[]>(new BYTE[256]());

  auto& ie = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(buffer.get());
  ie.mft_index = 42;
  ie.mft_sn = 1;

  auto& fn = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&ie.stream);
  fn.flags = NtfsBrowser::Flag::Filename::DIRECTORY;
  fn.name_length = static_cast<BYTE>(name.size());
  fn.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (size_t i = 0; i < name.size(); i++)
  {
    fn.name[i] = static_cast<WORD>(name[i]);
  }

  ie.stream_size =
      static_cast<WORD>(reinterpret_cast<BYTE*>(&fn.name[name.size()]) -
                        reinterpret_cast<BYTE*>(&fn));
  ie.size = static_cast<WORD>(reinterpret_cast<BYTE*>(&ie.stream) -
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
