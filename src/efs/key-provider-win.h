#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Efs {

// Unwraps FEKs with the keys of the certificates in one Windows certificate
// store. Owns the store and closes it. The factories of this provider,
// MakeCertStoreKeyProvider() and MakePfxKeyProvider(), are in efs.h.
class StoreKeyProvider final : public IEfsKeyProvider {
 public:
  // "store" is an HCERTSTORE, held as a plain pointer so this header does not
  // need wincrypt.h.
  explicit StoreKeyProvider(void* store) noexcept;
  StoreKeyProvider(StoreKeyProvider&& other) noexcept = delete;
  StoreKeyProvider(const StoreKeyProvider& other) = delete;
  StoreKeyProvider& operator=(StoreKeyProvider&& other) noexcept = delete;
  StoreKeyProvider& operator=(const StoreKeyProvider& other) = delete;
  ~StoreKeyProvider() override;

  [[nodiscard]] std::optional<std::vector<BYTE>>
      UnwrapFek(std::span<const BYTE> thumbprint,
                std::span<const BYTE> wrapped_fek) const override;

 private:
  void* store_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Efs
