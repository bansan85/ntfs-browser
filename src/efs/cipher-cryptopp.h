#pragma once

#include <memory>

#include <ntfs-browser/namespace.h>

#include "../internal-export.h"
#include "efs/sector-cipher.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Efs {

class Fek;

}  // namespace Efs
}  // namespace NTFS_BROWSER_NAMESPACE

#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP

namespace NTFS_BROWSER_NAMESPACE::Efs {

// The Crypto++ backend. Null if the key is unusable.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::unique_ptr<SectorDecryptor>
    MakeCryptoPpDecryptor(const Fek& fek);

}  // namespace NTFS_BROWSER_NAMESPACE::Efs

#endif
