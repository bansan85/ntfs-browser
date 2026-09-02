#pragma once

// NTFS_BROWSER_ENABLE_TRACE swaps NTFS_TRACE* no-ops for real printf output.
#ifdef NTFS_BROWSER_ENABLE_TRACE

  #include <cstdio>

  // Unlike NTFS_TRACE1-4, t1 can be caller data, not a printf format string.
  #define NTFS_TRACE(t1) printf("%s", static_cast<const char*>(t1))
  #define NTFS_TRACE1(t1, t2) printf(t1, t2)
  #define NTFS_TRACE2(t1, t2, t3) printf(t1, t2, t3)
  #define NTFS_TRACE3(t1, t2, t3, t4) printf(t1, t2, t3, t4)
  #define NTFS_TRACE4(t1, t2, t3, t4, t5) printf(t1, t2, t3, t4, t5)

#else

  #define NTFS_TRACE(t1)
  #define NTFS_TRACE1(t1, t2)
  #define NTFS_TRACE2(t1, t2, t3)
  #define NTFS_TRACE3(t1, t2, t3, t4)
  #define NTFS_TRACE4(t1, t2, t3, t4, t5)

#endif
