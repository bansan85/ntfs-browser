/*
 * NTFS Class common definitions
 * 
 * Copyright(C) 2010 cyb70289 <cyb70289@gmail.com>
 */

#ifndef __NTFS_COMMON_H_CYB70289
#define __NTFS_COMMON_H_CYB70289

#include <windows.h>

#include <ntfs-browser/data/attr-type.h>

#define ATTR_INDEX(at) \
  (((at) >> 4) - 1)  // Attribute Type to Index, eg. 0x10->0, 0x30->2
#define ATTR_MASK(at) (((DWORD)1) << ATTR_INDEX(at))  // Attribute Bit Mask

// Bit masks of Attributes
#define MASK_STANDARD_INFORMATION \
  ATTR_MASK(static_cast<DWORD>(AttrType::STANDARD_INFORMATION))
#define MASK_ATTRIBUTE_LIST \
  ATTR_MASK(static_cast<DWORD>(AttrType::ATTRIBUTE_LIST))
#define MASK_FILE_NAME ATTR_MASK(static_cast<DWORD>(AttrType::FILE_NAME))
#define MASK_OBJECT_ID ATTR_MASK(static_cast<DWORD>(AttrType::OBJECT_ID))
#define MASK_SECURITY_DESCRIPTOR \
  ATTR_MASK(static_cast<DWORD>(AttrType::SECURITY_DESCRIPTOR))
#define MASK_VOLUME_NAME ATTR_MASK(static_cast<DWORD>(AttrType::VOLUME_NAME))
#define MASK_VOLUME_INFORMATION \
  ATTR_MASK(static_cast<DWORD>(AttrType::VOLUME_INFORMATION))
#define MASK_DATA ATTR_MASK(static_cast<DWORD>(AttrType::DATA))
#define MASK_INDEX_ROOT ATTR_MASK(static_cast<DWORD>(AttrType::INDEX_ROOT))
#define MASK_INDEX_ALLOCATION \
  ATTR_MASK(static_cast<DWORD>(AttrType::INDEX_ALLOCATION))
#define MASK_BITMAP ATTR_MASK(static_cast<DWORD>(AttrType::BITMAP))
#define MASK_REPARSE_POINT \
  ATTR_MASK(static_cast<DWORD>(AttrType::REPARSE_POINT))
#define MASK_EA_INFORMATION \
  ATTR_MASK(static_cast<DWORD>(AttrType::EA_INFORMATION))
#define MASK_EA ATTR_MASK(static_cast<DWORD>(AttrType::EA))
#define MASK_LOGGED_UTILITY_STREAM \
  ATTR_MASK(static_cast<DWORD>(AttrType::LOGGED_UTILITY_STREAM))

#define MASK_ALL ((DWORD)-1)

#define NTFS_TRACE(t1) _RPT0(_CRT_WARN, t1)
#define NTFS_TRACE1(t1, t2) _RPT1(_CRT_WARN, t1, t2)
#define NTFS_TRACE2(t1, t2, t3) _RPT2(_CRT_WARN, t1, t2, t3)
#define NTFS_TRACE3(t1, t2, t3, t4) _RPT3(_CRT_WARN, t1, t2, t3, t4)
#define NTFS_TRACE4(t1, t2, t3, t4, t5) _RPT4(_CRT_WARN, t1, t2, t3, t4, t5)

#endif
