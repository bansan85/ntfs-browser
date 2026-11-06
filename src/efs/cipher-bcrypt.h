#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <span>

#include <ntfs-browser/namespace.h>

#include "../internal-export.h"
#include "efs/sector-cipher.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Efs {

class Fek;

}  // namespace Efs
}  // namespace NTFS_BROWSER_NAMESPACE

#if defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)

namespace NTFS_BROWSER_NAMESPACE::Efs {

// BCrypt in CBC mode, restarted with a fresh IV per sector. Owns its
// algorithm provider and key handles. They are held as plain pointers so this
// header does not need bcrypt.h.
class BCryptDecryptor final : public SectorDecryptor {
 public:
  BCryptDecryptor() = default;
  BCryptDecryptor(BCryptDecryptor&& other) noexcept = delete;
  BCryptDecryptor(const BCryptDecryptor& other) = delete;
  BCryptDecryptor& operator=(BCryptDecryptor&& other) noexcept = delete;
  BCryptDecryptor& operator=(const BCryptDecryptor& other) = delete;
  ~BCryptDecryptor() override;

  // Opens the algorithm in CBC mode and imports the key. Returns false if
  // BCrypt refuses either.
  [[nodiscard]] bool Init(const wchar_t* algorithm_id,
                          std::span<const BYTE> key, size_t block_size);

  [[nodiscard]] bool DecryptSector(ULONGLONG offset,
                                   std::span<BYTE> sector) const override;

 private:
  void* alg_{nullptr};
  void* key_{nullptr};
  size_t block_size_{0};
};

// The BCrypt backend. Null if the key is unusable, or for DESX, which BCrypt
// does not have.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::unique_ptr<SectorDecryptor>
    MakeBCryptDecryptor(const Fek& fek);

}  // namespace NTFS_BROWSER_NAMESPACE::Efs

#endif
