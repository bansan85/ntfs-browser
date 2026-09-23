#pragma once

#include <ntfs-browser/export.h>

// Marks an internal (non-public) symbol that the unit tests reach directly.
#ifdef NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS
  #define NTFS_BROWSER_EXPORT_TESTS_ONLY NTFS_BROWSER_EXPORT
#else
  #define NTFS_BROWSER_EXPORT_TESTS_ONLY
#endif
