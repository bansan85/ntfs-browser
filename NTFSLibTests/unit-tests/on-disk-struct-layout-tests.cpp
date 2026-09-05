// Regression tests asserting that a handful of on-disk struct layouts
// (src/data, src/attr) match their real, documented NTFS on-disk size -
// bugs here are easy to introduce (a stray non-bitfield member, missing
// #pragma pack) and easy to miss, since a self-sized test fixture built off
// the very same (possibly wrong) sizeof() stays "self-consistent" with the
// bug instead of exposing it (see attr-vol-info-size-tests.cpp and
// attribute-list-tests.cpp for the corresponding behavioral fixtures).

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>

#include "attr/attribute-list.h"

// Regression test for bug F11 (docs/bug-reports/2026-09-03-full-repo.md):
// Attr::MftSegmentReference (src/attr/attribute-list.h) used to declare
// "ULONGLONG segment_number : 48; WORD sequence_number;" - the WORD is NOT a
// bitfield, so it is placed AFTER the 8-byte allocation unit the bitfield
// opens, inflating sizeof() to 16 instead of the real on-disk 8-byte MFT
// segment/base file reference field. src/data/index-entry.h's IndexEntry
// uses the correct pattern for the exact same shape (two bitfields sharing
// one ULONGLONG allocation unit: mft_index:48 + mft_sn:16) - the fix here
// mirrors that: "ULONGLONG segment_number : 48; ULONGLONG sequence_number :
// 16;".
TEST_CASE(
    "MftSegmentReference is 8 bytes, matching the real on-disk base file "
    "reference field (F11)",
    "[attr-list][regression]")
{
  CHECK(sizeof(NtfsBrowser::Attr::MftSegmentReference) == 8);
}
