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

// Builds a single raw $I30 index entry named "System" (file reference 42),
// followed immediately in the same buffer by one filler UTF-16 code unit
// (0xFFFF) larger than any character a real name would contain.
//
// Filename::filename_wuc_ is a non-owning std::wstring_view straight into
// this kind of raw, non-null-terminated on-disk name buffer (see
// filename.h). Compare() used to call _wcsnicmp() with
// max(fn.size(), filename_wuc_.size()), which read past "System"'s 6 real
// characters into whatever bytes happened to follow whenever the entry name
// was a strict prefix of the search target - exactly this filler. Planting
// a deterministic, always-wrong value there makes any regression fail
// reliably, instead of depending on what garbage a real disk happens to
// leave adjacent to the name.
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
  fn.name[kNameLen] = 0xFFFF;  // filler: must never be read by Compare()

  ie.stream_size = static_cast<WORD>(reinterpret_cast<BYTE*>(&fn.name[kNameLen]) -
                                     reinterpret_cast<BYTE*>(&fn));
  ie.size = static_cast<WORD>(reinterpret_cast<BYTE*>(&ie.stream) -
                              reinterpret_cast<BYTE*>(&ie) + ie.stream_size);

  return IndexEntry(buffer, ie);
}

}  // namespace

// Regression test for the bug described above: searching a directory for
// "System32" used to wrongly stop at a "System" entry instead of continuing
// past it, because the out-of-bounds read could come back larger than the
// target's own character - eg. ".\NtfsDir.exe C:\windows\System32" failed
// with "Cannot find directory System32" even though C:\Windows\System32
// exists, precisely because C:\Windows also contains a "System" entry.
TEST_CASE("Compare treats a name as a prefix, not extended by trailing bytes",
          "[filename][regression]")
{
  const IndexEntry entry = MakeSystemEntry();
  REQUIRE(entry.HasName());
  REQUIRE(entry.GetFilename() == L"System");

  // "System32" is strictly longer than, and starts with, "System" - it must
  // sort after it, exactly like any other name sharing "System" as a
  // prefix (SystemApps, SystemResources, ...).
  CHECK(entry.Compare(L"System32") > 0);
  CHECK(entry.Compare(L"System") == 0);
}
