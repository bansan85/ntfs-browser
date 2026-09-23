#include <ntfs-browser/efs.h>

namespace NtfsBrowser::Efs
{

namespace
{
// Not atomic: like the logger, the library is single-threaded. Defaults to
// whichever backend is actually compiled in; this file is only compiled at
// all when at least one is, so one of the two branches below always applies.
#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP)
CipherBackend g_backend = CipherBackend::kCryptoPp;
#elif defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
CipherBackend g_backend = CipherBackend::kBCrypt;
#endif
}  // namespace

bool SetCipherBackend(CipherBackend backend) noexcept
{
  if (backend == CipherBackend::kCryptoPp)
  {
#ifndef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
    return false;
#endif
  }
  else if (backend == CipherBackend::kBCrypt)
  {
#if !(defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
    return false;
#endif
  }
  g_backend = backend;
  return true;
}

CipherBackend GetCipherBackend() noexcept { return g_backend; }

}  // namespace NtfsBrowser::Efs
