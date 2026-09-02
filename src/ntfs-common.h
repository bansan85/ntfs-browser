#pragma once

// Diagnostic tracing is compiled out by default (NTFS_TRACE* are no-ops) so
// normal consumers of the library never pay for it and never see it on
// stdout. NTFS_BROWSER_ENABLE_TRACE (set by NTFSLibTests, see
// NTFSLibTests/CMakeLists.txt) turns it into real printf output so tests can
// assert on the parse-error messages the library reports.
#ifdef NTFS_BROWSER_ENABLE_TRACE

  #include <cstdio>

  // t1 alone is caller-controlled in a few call sites (eg. exception::what()
  // messages) rather than always a literal, so it is never used as a printf
  // format string.
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
