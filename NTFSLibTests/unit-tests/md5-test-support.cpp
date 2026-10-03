#include "md5-test-support.h"

#include <ntfs-browser/win-types.h>

#ifdef NTFS_TEST_HAS_MD5

  #include <array>
  #include <format>

  #if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP)

  // NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
    #define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
    #include <cryptopp/md5.h>

  #elif defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)

    #include <bcrypt.h>
    #include <catch2/catch_test_macros.hpp>
    #include <gsl/narrow>

  #endif

namespace NtfsBrowserTests
{

namespace
{

std::string HexEncode(std::span<const BYTE> digest)
{
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const BYTE byte_value : digest)
  {
    hex += std::format("{:02x}", byte_value);
  }
  return hex;
}

}  // namespace

  #if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP)

std::string Md5Hex(std::span<const BYTE> data)
{
  std::array<BYTE, CryptoPP::Weak::MD5::DIGESTSIZE> digest{};
  CryptoPP::Weak::MD5().CalculateDigest(digest.data(), data.data(),
                                        data.size());
  return HexEncode(digest);
}

  #else

std::string Md5Hex(std::span<const BYTE> data)
{
  std::array<BYTE, 16> digest{};
  const NTSTATUS status = BCryptHash(
      BCRYPT_MD5_ALG_HANDLE, nullptr, 0, const_cast<BYTE*>(data.data()),
      gsl::narrow<ULONG>(data.size()), digest.data(),
      gsl::narrow<ULONG>(digest.size()));
  REQUIRE(BCRYPT_SUCCESS(status));
  return HexEncode(digest);
}

  #endif

}  // namespace NtfsBrowserTests

#endif  // NTFS_TEST_HAS_MD5
