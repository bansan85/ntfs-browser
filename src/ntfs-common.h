#pragma once

// OK

#include <crtdbg.h>

#define NTFS_TRACE(t1) _RPT0(_CRT_WARN, t1)
#define NTFS_TRACE1(t1, t2) _RPT1(_CRT_WARN, t1, t2)
#define NTFS_TRACE2(t1, t2, t3) _RPT2(_CRT_WARN, t1, t2, t3)
#define NTFS_TRACE3(t1, t2, t3, t4) _RPT3(_CRT_WARN, t1, t2, t3, t4)
#define NTFS_TRACE4(t1, t2, t3, t4, t5) _RPT4(_CRT_WARN, t1, t2, t3, t4, t5)
