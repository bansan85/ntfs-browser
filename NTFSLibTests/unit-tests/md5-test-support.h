#pragma once

#include <ntfs-browser/win-types.h>

#include <span>
#include <string>

// Set exactly when a build config makes Md5Hex() below available: mirrors
// src/efs/efs.cpp's own backend choice (Crypto++ over BCrypt over neither).
// MD5 here is only a fixture checksum, not library functionality.
#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  // NOLINTNEXTLINE(cppcoreguidelines-macro-usage): tested with #ifdef.
  #define NTFS_TEST_HAS_MD5 1
#endif

#ifdef NTFS_TEST_HAS_MD5

namespace NtfsBrowserTests {

// Hex-encodes the MD5 of data, using whichever EFS crypto backend this build
// already compiles in.
std::string Md5Hex(std::span<const BYTE> data);

}  // namespace NtfsBrowserTests

#endif  // NTFS_TEST_HAS_MD5
