#include <cstdlib>

#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>

// Exercises exported symbols from across the public API, to prove the
// library links correctly when consumed via FetchContent: DLL import/export
// with BUILD_SHARED_LIBS on, plus every optional feature this build enables.
int main() {
  NTFS_BROWSER_NAMESPACE::Log::Configure(NTFS_BROWSER_NAMESPACE::Log::Config{});

// Decompression and EFS decryption are both optional features.
#if defined(FETCHCONTENT_SMOKE_TEST_EFS) && defined(_WIN32)
  const auto provider = NTFS_BROWSER_NAMESPACE::Efs::MakeCertStoreKeyProvider();
  (void)provider;
#endif

  return EXIT_SUCCESS;
}
