#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>

#include "long-path-support.h"
#include "ntfsundel-core.h"
#include "temp-dir.h"
#include "text-convert.h"

namespace Fs = std::filesystem;
using NTFS_BROWSER_NAMESPACE::Tests::LongTempDir;
using NTFS_BROWSER_NAMESPACE::Tests::TempDir;
using NtfsUndel::CreatedFile;
using NtfsUndel::CreateError;
using NtfsUndel::CreateRecoveryFile;
using NtfsUndel::ExistingFile;
using NtfsUndel::NameChange;
using NtfsUndel::SafeFileName;

namespace {

// The bytes of a recovered file: what its name says it holds.
std::span<const BYTE> Bytes(std::string_view text) {
  return {reinterpret_cast<const BYTE*>(text.data()), text.size()};
}

// The content of the file at path, which may be longer than 260 units.
std::string ReadAll(const Fs::path& path) {
  std::ifstream input(NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(path),
                      std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

// What a recovery made: whether it made a file, and the path and name change
// of that file.
struct Recovered {
  bool made = false;
  Fs::path path;
  NameChange change = NameChange::None;
};

// Creates a file of content in directory, the way a recovery does, and closes
// it.
Recovered Recover(const Fs::path& directory, std::wstring_view name,
                  std::string_view content,
                  ExistingFile existing = ExistingFile::KeepBoth) {
  CreatedFile created = CreateRecoveryFile(directory.wstring(), name, existing);
  if (!created.file) {
    return {};
  }
  REQUIRE(created.file->Write(Bytes(content)));
  return {.made = true,
          .path = created.file->Path(),
          .change = created.file->Change()};
}

// Reports whether directory keeps two names that differ only by case apart: a
// directory that does not is the one where recovered names collide.
bool IsCaseInsensitive(const Fs::path& directory) {
  const Fs::path probe = directory / "case-probe";
  std::ofstream(probe).put('x');
  const bool same = Fs::exists(directory / "CASE-PROBE");
  Fs::remove(probe);
  return same;
}

}  // namespace

TEST_CASE("SafeFileName keeps a name Win32 can create", "[undel][file-name]") {
  for (const std::wstring_view name :
       {L"report.txt", L"café", L"a b", L"a.b.c", L"x", L"CONSOLE",
        L"conference.txt", L"COM10", L"LPT", L".hidden", L"a-b_c~1.TXT",
        L"日本.txt"}) {
    INFO(NtfsDevice::WideToUtf8(name));
    CHECK(SafeFileName(name) == name);
  }
}

TEST_CASE("SafeFileName drops the dots and spaces that end a name",
          "[undel][file-name]") {
  CHECK(SafeFileName(L"dot.") == L"dot");
  CHECK(SafeFileName(L"space ") == L"space");
  CHECK(SafeFileName(L"a. .") == L"a");
  CHECK(SafeFileName(L"inner. dot.txt") == L"inner. dot.txt");
}

TEST_CASE("SafeFileName gives an empty name a name", "[undel][file-name]") {
  CHECK(SafeFileName(L"") == L"_");
  CHECK(SafeFileName(L" ") == L"_");
  CHECK(SafeFileName(L"...") == L"_");
  CHECK(SafeFileName(L".") == L"_");
  CHECK(SafeFileName(L"..") == L"_");
}

TEST_CASE("SafeFileName replaces a character Win32 refuses",
          "[undel][file-name]") {
  CHECK(SafeFileName(LR"(a<b>c:d"e/f\g|h?i*j)") == L"a_b_c_d_e_f_g_h_i_j");
  CHECK(SafeFileName(std::wstring{L'a', L'\x01', L'b', L'\x1F', L'c'}) ==
        L"a_b_c");
  CHECK(SafeFileName(std::wstring{L'a', L'\0', L'b'}) == L"a_b");
  CHECK(SafeFileName(std::wstring{L'a', L'\n', L'b'}) == L"a_b");
}

TEST_CASE("SafeFileName puts a character before a device name",
          "[undel][file-name]") {
  CHECK(SafeFileName(L"CON") == L"_CON");
  CHECK(SafeFileName(L"con.txt") == L"_con.txt");
  CHECK(SafeFileName(L"Nul") == L"_Nul");
  CHECK(SafeFileName(L"PRN.tar.gz") == L"_PRN.tar.gz");
  CHECK(SafeFileName(L"aux") == L"_aux");
  CHECK(SafeFileName(L"COM1") == L"_COM1");
  CHECK(SafeFileName(L"com9.log") == L"_com9.log");
  CHECK(SafeFileName(L"LPT5") == L"_LPT5");
  CHECK(SafeFileName(L"CON .txt") == L"_CON .txt");
  CHECK(SafeFileName(std::wstring(L"COM") + static_cast<wchar_t>(0x00B9)) ==
        std::wstring(L"_COM") + static_cast<wchar_t>(0x00B9));
}

TEST_CASE("SafeFileName cuts a name at 255 units, never inside a pair",
          "[undel][file-name]") {
  const std::wstring long_name(300, L'a');
  CHECK(SafeFileName(long_name) == std::wstring(255, L'a'));

  // A character past the BMP takes two units: its first would be the 255th.
  const std::wstring pair_at_the_end =
      std::wstring(254, L'a') + L"\U0001F600" + L"tail";
  const std::wstring cut = SafeFileName(pair_at_the_end);
  if constexpr (sizeof(wchar_t) == 2) {
    CHECK(cut == std::wstring(254, L'a'));
  } else {
    CHECK(cut.size() == 255);
  }

  // The cut can leave a dot at the end.
  CHECK(SafeFileName(std::wstring(254, L'a') + L"..") ==
        std::wstring(254, L'a'));
}

TEST_CASE("SafeFileName keeps an unpaired surrogate", "[undel][file-name]") {
  const std::wstring name{L'x', static_cast<wchar_t>(0xD800), L'y'};

  CHECK(SafeFileName(name) == name);
}

TEST_CASE("CreateRecoveryFile creates a file and writes into it",
          "[undel][file]") {
  const TempDir directory;

  {
    const Recovered created = Recover(directory.Path(), L"report.txt", "data");
    REQUIRE(created.made);
    CHECK(created.change == NameChange::None);
    CHECK(created.path == directory.Path() / L"report.txt");
  }

  CHECK(ReadAll(directory.Path() / L"report.txt") == "data");
}

TEST_CASE("CreateRecoveryFile keeps a file that exists and numbers the copy",
          "[undel][file]") {
  const TempDir directory;
  const Recovered first = Recover(directory.Path(), L"a.txt", "first");
  const Recovered second = Recover(directory.Path(), L"a.txt", "second");
  const Recovered third = Recover(directory.Path(), L"a.txt", "third");

  REQUIRE(first.made);
  REQUIRE(second.made);
  REQUIRE(third.made);
  CHECK(first.change == NameChange::None);
  CHECK(second.change == NameChange::Taken);
  CHECK(third.change == NameChange::Taken);
  CHECK(Fs::path(second.path).filename() == L"a (2).txt");
  CHECK(Fs::path(third.path).filename() == L"a (3).txt");
  CHECK(ReadAll(directory.Path() / L"a.txt") == "first");
  CHECK(ReadAll(directory.Path() / L"a (2).txt") == "second");
  CHECK(ReadAll(directory.Path() / L"a (3).txt") == "third");
}

TEST_CASE("CreateRecoveryFile numbers a name that has no extension",
          "[undel][file]") {
  const TempDir directory;
  const Recovered first = Recover(directory.Path(), L"notes", "1");
  const Recovered second = Recover(directory.Path(), L"notes", "2");
  const Recovered hidden_first = Recover(directory.Path(), L".profile", "1");
  const Recovered hidden_second = Recover(directory.Path(), L".profile", "2");

  REQUIRE(second.made);
  REQUIRE(hidden_second.made);
  CHECK(Fs::path(second.path).filename() == L"notes (2)");
  CHECK(Fs::path(hidden_second.path).filename() == L".profile (2)");
}

TEST_CASE(
    "CreateRecoveryFile does not overwrite a name that differs only by "
    "case",
    "[undel][file][case]") {
  const TempDir directory;
  if (!IsCaseInsensitive(directory.Path())) {
    SKIP("the temp directory tells names that differ by case apart");
  }

  const Recovered lower = Recover(directory.Path(), L"readme", "lower");
  const Recovered upper = Recover(directory.Path(), L"README", "upper");
  const Recovered mixed = Recover(directory.Path(), L"ReadMe", "mixed");

  REQUIRE(lower.made);
  REQUIRE(upper.made);
  REQUIRE(mixed.made);
  CHECK(lower.change == NameChange::None);
  CHECK(upper.change == NameChange::Taken);
  CHECK(mixed.change == NameChange::Taken);
  CHECK(Fs::path(upper.path).filename() == L"README (2)");
  CHECK(Fs::path(mixed.path).filename() == L"ReadMe (3)");
  CHECK(ReadAll(directory.Path() / L"readme") == "lower");
  CHECK(ReadAll(directory.Path() / L"README (2)") == "upper");
  CHECK(ReadAll(directory.Path() / L"ReadMe (3)") == "mixed");
}

TEST_CASE("CreateRecoveryFile replaces a file when asked to", "[undel][file]") {
  const TempDir directory;
  const Recovered old_file = Recover(directory.Path(), L"a.txt", "old old");
  const Recovered replacement =
      Recover(directory.Path(), L"a.txt", "new", ExistingFile::Replace);

  REQUIRE(replacement.made);
  CHECK(replacement.change == NameChange::None);
  CHECK(ReadAll(directory.Path() / L"a.txt") == "new");
}

TEST_CASE("CreateRecoveryFile creates a name Win32 would not take as it is",
          "[undel][file]") {
  const TempDir directory;

  const Recovered device = Recover(directory.Path(), L"CON", "device");
  const Recovered dot = Recover(directory.Path(), L"dot.", "dot");
  const Recovered slash = Recover(directory.Path(), L"a:b", "colon");

  REQUIRE(device.made);
  REQUIRE(dot.made);
  REQUIRE(slash.made);
  CHECK(device.change == NameChange::Unsafe);
  CHECK(dot.change == NameChange::Unsafe);
  CHECK(slash.change == NameChange::Unsafe);
  CHECK(Fs::path(device.path).filename() == L"_CON");
  CHECK(Fs::path(dot.path).filename() == L"dot");
  CHECK(Fs::path(slash.path).filename() == L"a_b");
  CHECK(ReadAll(directory.Path() / L"_CON") == "device");
  CHECK(ReadAll(directory.Path() / L"dot") == "dot");
  CHECK(ReadAll(directory.Path() / L"a_b") == "colon");
}

TEST_CASE("CreateRecoveryFile says Unsafe and then Taken apart",
          "[undel][file]") {
  const TempDir directory;
  const Recovered first = Recover(directory.Path(), L"dot", "1");
  const Recovered second = Recover(directory.Path(), L"dot.", "2");

  REQUIRE(second.made);
  CHECK(second.change == NameChange::Taken);
  CHECK(Fs::path(second.path).filename() == L"dot (2)");
}

TEST_CASE("CreateRecoveryFile creates a file at a path of more than 260 units",
          "[undel][file][long-path]") {
  const LongTempDir directory(300);

  {
    const Recovered created =
        Recover(directory.Directory(), std::wstring(120, L'n') + L".bin", "x");
    REQUIRE(created.made);
    CHECK(created.change == NameChange::None);
    CHECK(created.path.native().size() > 260);
    // The path a user sees has no prefix.
    CHECK(created.path.native().substr(0, 4) != LR"(\\?\)");
  }

  CHECK(ReadAll(directory.Directory() / (std::string(120, 'n') + ".bin")) ==
        "x");
}

TEST_CASE("CreateRecoveryFile numbers a name at a path of more than 260 units",
          "[undel][file][long-path]") {
  const LongTempDir directory(300);
  const std::wstring name = std::wstring(120, L'n') + L".bin";
  const Recovered first = Recover(directory.Directory(), name, "1");
  const Recovered second = Recover(directory.Directory(), name, "2");

  REQUIRE(second.made);
  CHECK(second.change == NameChange::Taken);
  CHECK(ReadAll(Fs::path(second.path)) == "2");
}

TEST_CASE("CreateRecoveryFile refuses a path that no Win32 call takes",
          "[undel][file]") {
  const std::wstring directory(33000, L'd');

  const CreatedFile created =
      CreateRecoveryFile(directory, L"a.txt", ExistingFile::KeepBoth);

  CHECK_FALSE(created.file);
  CHECK(created.error == CreateError::PathTooLong);
}

TEST_CASE("CreateRecoveryFile fails in a folder that is not there",
          "[undel][file]") {
  const TempDir directory;

  const CreatedFile created =
      CreateRecoveryFile((directory.Path() / L"missing").wstring(), L"a.txt",
                         ExistingFile::KeepBoth);

  CHECK_FALSE(created.file);
  CHECK(created.error == CreateError::Failed);
}

TEST_CASE("a RecoveryFile that moves keeps the file open once",
          "[undel][file]") {
  const TempDir directory;
  CreatedFile created = CreateRecoveryFile(
      directory.Path().wstring(), L"moved.txt", ExistingFile::KeepBoth);
  REQUIRE(created.file);

  NtfsUndel::RecoveryFile moved = std::move(*created.file);
  CHECK(moved.Write(Bytes("moved")));
  CHECK_FALSE(created.file->Write(Bytes("moved-from")));

  NtfsUndel::RecoveryFile assigned = std::move(moved);
  CHECK(assigned.Write(Bytes(" twice")));
  CHECK_FALSE(moved.Write(Bytes("nothing")));
}
