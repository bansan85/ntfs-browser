#pragma once

// The test seam of Search::Reader. A test sets it while no Reader works.
// Without it a Reader uses the key provider that its volume makes by itself.

#include <memory>
#include <optional>

#include <ntfs-browser/namespace.h>

#include "efs/efs.h"
#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// Makes every Reader that opens from now on give provider to its volume as the
// source of EFS keys. A null provider turns decryption off. The default
// provider reads the certificate store of the current user: a test MUST set
// this seam, or a null provider, before it reads an encrypted file. Not thread
// safe: call it while no Reader works.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SetReaderKeyProviderForTests(
    std::shared_ptr<NTFS_BROWSER_NAMESPACE::Efs::IEfsKeyProvider> provider);

// Takes the seam away: the next Readers use the default provider again.
NTFS_BROWSER_EXPORT_TESTS_ONLY void ClearReaderKeyProviderForTests();

// The provider that the seam holds, or nothing. A set value that holds a null
// pointer turns decryption off.
[[nodiscard]] const std::optional<
    std::shared_ptr<NTFS_BROWSER_NAMESPACE::Efs::IEfsKeyProvider>>&
    ReaderKeyProviderOverride() noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Search
