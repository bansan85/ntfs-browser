#include <ntfs-browser/efs.h>

namespace NtfsBrowser::Efs
{

namespace
{
// Not atomic: like the logger, the library is single-threaded.
CipherBackend g_backend = CipherBackend::kCryptoPp;
}  // namespace

bool SetCipherBackend(CipherBackend backend) noexcept
{
#ifndef _WIN32
  if (backend == CipherBackend::kBCrypt)
  {
    return false;
  }
#endif
  g_backend = backend;
  return true;
}

CipherBackend GetCipherBackend() noexcept { return g_backend; }

}  // namespace NtfsBrowser::Efs
