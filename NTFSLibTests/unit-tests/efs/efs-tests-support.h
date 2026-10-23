#pragma once

#include <ntfs-browser/win-types.h>

#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "efs-test-support.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

template <Cache::Strategy S>
class AttrBase;

}  // namespace Attr

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Efs {

class IEfsKeyProvider;
enum class Algorithm : WORD;
enum class CipherBackend : std::uint8_t;

}  // namespace Efs

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

using NTFS_BROWSER_NAMESPACE::Attr::AttrBase;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Efs::CipherBackend;
using NTFS_BROWSER_NAMESPACE::Efs::IEfsKeyProvider;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::Tests::Algorithm;
using NTFS_BROWSER_NAMESPACE::Tests::TestEfsEntry;
using NTFS_BROWSER_NAMESPACE::Tests::TestKeyProvider;

namespace {

// The wrapped FEK the fixtures store. The test provider matches it byte for
// byte, so its content only has to be recognisable.
constexpr size_t wrapped_fek_size = 32;

constexpr BYTE wrapped_fek_fill = 0xAB;

const std::vector<BYTE> wrapped_fek(wrapped_fek_size, wrapped_fek_fill);

// Plaintext size of a default fixture file: several sectors, not a multiple
// of one.
constexpr size_t default_file_size = 3000;

// Fill that tells an unread byte from a read one.
constexpr BYTE unread_fill = 0xCC;

// Offset of the algorithm DWORD in a FEK blob, and an unsupported key length.
constexpr size_t fek_algorithm_field = 8;

// A cluster of ciphertext that is never decrypted, and the size it claims.
constexpr size_t bogus_cluster_size = 1024;

constexpr BYTE bogus_cluster_fill = 7;

constexpr ULONGLONG bogus_real_size = 1000;

// Where the first data stream of a fixture starts on disk, in clusters.
constexpr DWORD first_stream_lcn = 30;

// The user every fixture encrypts for.
inline TestEfsEntry TestUser(BYTE seed = 1,
                             std::vector<BYTE> wrapped = wrapped_fek) {
  return {NTFS_BROWSER_NAMESPACE::Tests::TestThumbprint(seed),
          std::move(wrapped)};
}

// A key provider that knows one key: the FEK of entry, for algorithm.
inline std::shared_ptr<TestKeyProvider>
    ProviderFor(const TestEfsEntry& entry,
                NTFS_BROWSER_NAMESPACE::Tests::Algorithm algorithm,
                const std::vector<BYTE>& key) {
  auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(entry.thumbprint, entry.wrapped_fek,
                NTFS_BROWSER_NAMESPACE::Tests::MakeFekBlob(algorithm, key));
  return provider;
}

inline DWORD ClustersFor(size_t bytes) {
  return gsl::narrow<DWORD>(
      (bytes + NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size - 1) /
      NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
}

// An encrypted file of one unnamed stream, its key provider, and the
// plaintext the stream must read back as.
struct Fixture {
  std::vector<BYTE> plaintext;
  std::vector<BYTE> image;
  std::shared_ptr<TestKeyProvider> provider;
};

inline Fixture MakeFixture(Algorithm algorithm,
                           size_t size = default_file_size) {
  Fixture fixture;
  fixture.plaintext = NTFS_BROWSER_NAMESPACE::Tests::PlaintextPattern(size);
  const std::vector<BYTE> key =
      NTFS_BROWSER_NAMESPACE::Tests::TestKey(algorithm);

  const TestEfsEntry user = TestUser();
  fixture.provider = ProviderFor(user, algorithm, key);

  NTFS_BROWSER_NAMESPACE::Tests::FakeEncryptedFile file;
  file.efs_stream =
      NTFS_BROWSER_NAMESPACE::Tests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back(
      {.runs = {{first_stream_lcn, ClustersFor(size)}},
       .cluster_bytes = NTFS_BROWSER_NAMESPACE::Tests::EfsEncrypt(
           algorithm, key, fixture.plaintext),
       .real_size = size});
  fixture.image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithEncryptedFile(file);
  return fixture;
}

template <Cache::Strategy S>
struct Opened {
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>> record;
};

// Opens the image and parses the root record. The provider is always set,
// null included, so that no test ever reaches the real certificate store.
// "mask" is nullopt for the default Mask::ALL; passing one exercises the
// same SetAttrMask() a caller narrowing to Mask::DATA would use. "options"
// defaults to strict; a test of a salvageable condition passes recover_errors.
// "backend" is nullopt for the volume's default.
template <Cache::Strategy S>
Opened<S> OpenVolume(std::vector<BYTE> image,
                     std::shared_ptr<IEfsKeyProvider> provider,
                     std::optional<Attr::Mask> mask = std::nullopt,
                     const VolumeOptions& options = {},
                     std::optional<CipherBackend> backend = std::nullopt) {
  Opened<S> opened;
  opened.volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          std::move(image)),
      options);
  REQUIRE(opened.volume->IsVolumeOK());
  opened.volume->SetEfsKeyProvider(std::move(provider));
  if (backend) {
    REQUIRE(opened.volume->SetEfsCipherBackend(*backend));
  }

  opened.record = std::make_unique<NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>>(
      *opened.volume);
  if (mask) {
    opened.record->SetAttrMask(*mask);
  }
  REQUIRE(
      opened.record->ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(opened.record->ParseAttrs());
  return opened;
}

// Reads up to "size" bytes at "offset". Nullopt if ReadData() failed.
template <Cache::Strategy S>
std::optional<std::vector<BYTE>>
    ReadAt(const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<S>& attr,
           ULONGLONG offset, size_t size) {
  std::vector<BYTE> buffer(size, unread_fill);
  const std::optional<ULONGLONG> read = attr.ReadData(offset, buffer);
  if (!read) {
    return std::nullopt;
  }
  buffer.resize(gsl::narrow<size_t>(*read));
  return buffer;
}

template <Cache::Strategy S>
const NTFS_BROWSER_NAMESPACE::Attr::AttrBase<S>&
    OnlyData(const NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>& record) {
  const auto& data = record.GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);
  return *data.front();
}

inline void Patch32(std::vector<BYTE>& bytes, size_t offset, DWORD value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(value));
}

}  // namespace
