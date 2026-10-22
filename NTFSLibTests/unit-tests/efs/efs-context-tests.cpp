#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/namespace.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "efs-test-support.h"
#include "efs/efs-context.h"
#include "efs/efs-stream.h"
#include "efs/fek.h"
#include "efs/sector-cipher.h"
#include "test-log-sink.h"

namespace Efs = NTFS_BROWSER_NAMESPACE::Efs;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using Catch::Matchers::ContainsSubstring;
using Tests::Algorithm;
using Tests::TestKeyProvider;

namespace {

// A backend this build has: Crypto++ when it is compiled in, else BCrypt.
constexpr Efs::CipherBackend backend =
#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
    Efs::CipherBackend::CryptoPp;
#else
    Efs::CipherBackend::BCrypt;
#endif

// The user the entries name, and the FEK wrapped for them.
const std::vector<BYTE> user_thumbprint(Tests::thumbprint_size_value, 0x11);
const std::vector<BYTE> user_wrapped_fek{0xAA, 0xBB, 0xCC};

// A context for one entry, whose provider hands out an AES-256 key.
Efs::Context MakeContext() {
  auto provider = std::make_shared<TestKeyProvider>();
  std::array<BYTE, Tests::thumbprint_size_value> thumbprint{};
  std::ranges::copy(user_thumbprint, thumbprint.begin());
  provider->Add(
      thumbprint, user_wrapped_fek,
      Tests::MakeFekBlob(Algorithm::Aes256, Tests::TestKey(Algorithm::Aes256)));
  return Efs::Context(
      {Efs::WrappedFek{.thumbprint = user_thumbprint,
                       .wrapped_fek = user_wrapped_fek}},
      [provider]() -> std::shared_ptr<Efs::IEfsKeyProvider> {
        return provider;
      },
      backend);
}

}  // namespace

TEST_CASE("A context refuses data that is not whole sectors at a sector start",
          "[efs][efs-context]") {
  const Efs::Context context = MakeContext();
  static_cast<void>(Tests::TakeCapturedLog());

  SECTION("a size that is not a multiple of a sector") {
    std::vector<BYTE> data(Efs::sector_size + 1, 0);
    CHECK_FALSE(context.Decrypt(0, data));
  }
  SECTION("an offset inside a sector") {
    std::vector<BYTE> data(Efs::sector_size, 0);
    CHECK_FALSE(context.Decrypt(1, data));
  }

  CHECK_THAT(Tests::TakeCapturedLog(), ContainsSubstring("not sector aligned"));
}

TEST_CASE("A context decrypts whole sectors at a sector start",
          "[efs][efs-context]") {
  const Efs::Context context = MakeContext();
  const std::vector<BYTE> plaintext =
      Tests::PlaintextPattern(2 * Efs::sector_size);
  std::vector<BYTE> data =
      Tests::EfsEncrypt(Algorithm::Aes256, Tests::TestKey(Algorithm::Aes256),
                        plaintext, Efs::sector_size);

  REQUIRE(context.Decrypt(Efs::sector_size, data));
  CHECK(data == plaintext);
}

TEST_CASE("A context without a provider reports why it cannot decrypt",
          "[efs][efs-context]") {
  const Efs::Context context(
      {Efs::WrappedFek{.thumbprint = user_thumbprint,
                       .wrapped_fek = user_wrapped_fek}},
      []() -> std::shared_ptr<Efs::IEfsKeyProvider> { return nullptr; },
      backend);
  static_cast<void>(Tests::TakeCapturedLog());
  std::vector<BYTE> data(Efs::sector_size, 0);

  CHECK_FALSE(context.Decrypt(0, data));

  CHECK_THAT(Tests::TakeCapturedLog(),
             ContainsSubstring("no EFS key provider is installed"));
}
