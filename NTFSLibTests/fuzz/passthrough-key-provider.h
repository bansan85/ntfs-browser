#pragma once

#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "efs/efs.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Fuzz {

// Stands in for a certificate store: the "wrapped" FEK the input stores is
// returned as if it were already decrypted. The input then picks the cipher
// and the key itself. A thumbprint that starts with refused_thumbprint models
// a key the provider does not hold.
class PassthroughKeyProvider final
    : public NTFS_BROWSER_NAMESPACE::Efs::IEfsKeyProvider {
 public:
  // The wrapped FEK as it is, or nothing for a refused thumbprint.
  [[nodiscard]] std::optional<std::vector<BYTE>>
      UnwrapFek(std::span<const BYTE> thumbprint,
                std::span<const BYTE> wrapped_fek) const override {
    if (!thumbprint.empty() && thumbprint.front() == refused_thumbprint) {
      return std::nullopt;
    }
    return std::vector<BYTE>(wrapped_fek.begin(), wrapped_fek.end());
  }

  // First byte of a thumbprint this provider refuses to unwrap.
  static constexpr BYTE refused_thumbprint = 0xFF;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Fuzz
