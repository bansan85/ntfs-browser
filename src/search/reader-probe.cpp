#include "search/reader-probe.h"

#include <memory>
#include <optional>
#include <utility>

#include <ntfs-browser/namespace.h>

#include "efs/efs.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

namespace {

// The provider of SetReaderKeyProviderForTests(). Empty in every real use. A
// set optional that holds a null pointer is a provider that decrypts nothing.
std::optional<std::shared_ptr<Efs::IEfsKeyProvider>>& KeyProviderStorage() {
  static std::optional<std::shared_ptr<Efs::IEfsKeyProvider>> provider;
  return provider;
}

}  // namespace

void SetReaderKeyProviderForTests(
    std::shared_ptr<Efs::IEfsKeyProvider> provider) {
  KeyProviderStorage() = std::move(provider);
}

void ClearReaderKeyProviderForTests() { KeyProviderStorage().reset(); }

const std::optional<std::shared_ptr<Efs::IEfsKeyProvider>>&
    ReaderKeyProviderOverride() noexcept {
  return KeyProviderStorage();
}

}  // namespace NTFS_BROWSER_NAMESPACE::Search
