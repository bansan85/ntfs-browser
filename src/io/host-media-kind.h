#pragma once

#include <string>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"
#include "tuning/tuning.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// The kind of device a handle names, from its seek penalty (IOCTL_STORAGE_
// QUERY_PROPERTY). handle is a HANDLE. Unknown when the handle refuses the
// query, as the handle of a plain file does.
[[nodiscard]] Tuning::MediaKind SeekPenaltyKindOf(void* handle) noexcept;

// Answers for a plain file: the kind of the volume that hosts it. path is the
// extended-length path the file was opened through.
using HostMediaQuery = Tuning::MediaKind (*)(const std::wstring& path);

// The kind of device behind the volume that hosts the file at path, as the
// installed query says. Unknown when the volume does not say. Windows only
// in practice: no other platform reads its files through Win32DiskReader.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY Tuning::MediaKind
    QueryHostMediaKind(const std::wstring& path);

// Installs query as the answer of QueryHostMediaKind() and returns the one it
// replaces. Null restores the real query, which opens the host volume. The
// unit tests install one that opens nothing before main(), so that no test
// depends on the disk of the machine or opens a device.
NTFS_BROWSER_EXPORT_TESTS_ONLY HostMediaQuery
    SetHostMediaQueryForTests(HostMediaQuery query) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
