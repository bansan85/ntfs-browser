#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>

using NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath;
using NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthPath;

#ifdef _WIN32

TEST_CASE("ToExtendedLengthPath prefixes an absolute drive path",
          "[io][extended-path]") {
  CHECK(ToExtendedLengthPath(LR"(C:\dir\image.img)") ==
        LR"(\\?\C:\dir\image.img)");
}

TEST_CASE("ToExtendedLengthPath resolves dots and forward slashes",
          "[io][extended-path]") {
  CHECK(ToExtendedLengthPath(L"C:/dir/./sub/../image.img") ==
        LR"(\\?\C:\dir\image.img)");
}

TEST_CASE("ToExtendedLengthPath makes a relative path absolute",
          "[io][extended-path]") {
  const std::wstring extended = ToExtendedLengthPath(LR"(sub\..\image.img)");

  CHECK(extended ==
        std::wstring(LR"(\\?\)") +
            (std::filesystem::current_path() / L"image.img").native());
}

TEST_CASE("ToExtendedLengthPath turns a UNC path into the UNC form",
          "[io][extended-path]") {
  CHECK(ToExtendedLengthPath(LR"(\\server\share\dir\image.img)") ==
        LR"(\\?\UNC\server\share\dir\image.img)");
}

TEST_CASE("ToExtendedLengthPath leaves an extended path as it is",
          "[io][extended-path]") {
  CHECK(ToExtendedLengthPath(LR"(\\?\C:\dir\image.img)") ==
        LR"(\\?\C:\dir\image.img)");
  CHECK(ToExtendedLengthPath(LR"(\\?\UNC\server\share\x)") ==
        LR"(\\?\UNC\server\share\x)");
  // The prefix says that the rest is not to be normalised.
  CHECK(ToExtendedLengthPath(LR"(\\?\C:\a\..\b.)") == LR"(\\?\C:\a\..\b.)");
}

TEST_CASE("ToExtendedLengthPath leaves a device path as it is",
          "[io][extended-path]") {
  CHECK(ToExtendedLengthPath(LR"(\\.\PhysicalDrive0)") ==
        LR"(\\.\PhysicalDrive0)");
  CHECK(ToExtendedLengthPath(LR"(\\.\C:)") == LR"(\\.\C:)");
}

TEST_CASE("ToExtendedLengthPath keeps a reserved device name a device",
          "[io][extended-path]") {
  const std::wstring extended = ToExtendedLengthPath(L"NUL");

  CHECK(extended == LR"(\\.\NUL)");
}

TEST_CASE("ToExtendedLengthPath drops a trailing dot and space",
          "[io][extended-path]") {
  CHECK(ToExtendedLengthPath(L"C:\\dir\\image.img. ") ==
        LR"(\\?\C:\dir\image.img)");
}

TEST_CASE("ToExtendedLengthPath keeps an empty path empty",
          "[io][extended-path]") {
  CHECK(ToExtendedLengthPath(L"").empty());
}

TEST_CASE("ToExtendedLengthPath takes a path of more than 260 units",
          "[io][extended-path]") {
  const std::wstring long_name(300, L'n');
  const std::wstring plain = L"C:\\dir\\" + long_name + L".img";

  const std::wstring extended = ToExtendedLengthPath(plain);

  CHECK(extended == LR"(\\?\)" + plain);
  CHECK(extended.size() > 260);
}

TEST_CASE("ToExtendedLengthPath takes a path of more than 512 units",
          "[io][extended-path]") {
  // More than the first buffer of the conversion.
  std::wstring plain = L"C:";
  for (int level = 0; level < 12; level++) {
    plain += L"\\" + std::wstring(100, static_cast<wchar_t>(L'a' + level));
  }

  CHECK(ToExtendedLengthPath(plain) == LR"(\\?\)" + plain);
}

TEST_CASE("ToExtendedLengthFsPath converts a std::filesystem path the same",
          "[io][extended-path]") {
  const std::filesystem::path plain = LR"(C:\dir\image.img)";

  CHECK(ToExtendedLengthFsPath(plain).native() ==
        ToExtendedLengthPath(plain.native()));
}

#else

TEST_CASE("ToExtendedLengthPath changes nothing off Windows",
          "[io][extended-path]") {
  CHECK(ToExtendedLengthPath(L"/dir/../image.img") == L"/dir/../image.img");
  CHECK(ToExtendedLengthPath(L"relative/image.img") == L"relative/image.img");
  CHECK(ToExtendedLengthPath(L"").empty());
  CHECK(ToExtendedLengthFsPath(std::filesystem::path("/a/../b")) ==
        std::filesystem::path("/a/../b"));
}

#endif  // _WIN32
