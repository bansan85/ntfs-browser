#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/efs.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "efs-test-support.h"
#include "efs/efs-stream.h"
#include "efs/fek.h"
#include "efs/sector-cipher.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using NtfsBrowser::AttrBase;
using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Efs::CipherBackend;
using NtfsBrowser::Efs::Fek;
using NtfsBrowser::Efs::IEfsKeyProvider;
using NtfsBrowser::Enum::MftIdx;
using NtfsBrowserTests::Algorithm;
using NtfsBrowserTests::TestEfsEntry;
using NtfsBrowserTests::TestKeyProvider;

namespace
{

// The wrapped FEK the fixtures store. The test provider matches it byte for
// byte, so its content only has to be recognisable.
const std::vector<BYTE> kWrappedFek(32, 0xAB);

// Where the first data stream of a fixture starts on disk, in clusters.
constexpr DWORD kFirstStreamLcn = 30;

// The user every fixture encrypts for.
TestEfsEntry TestUser(BYTE seed = 1, std::vector<BYTE> wrapped = kWrappedFek)
{
  return {NtfsBrowserTests::TestThumbprint(seed), std::move(wrapped)};
}

DWORD ClustersFor(size_t bytes)
{
  return static_cast<DWORD>((bytes + NtfsBrowserTests::kFakeClusterSize - 1) /
                            NtfsBrowserTests::kFakeClusterSize);
}

// An encrypted file of one unnamed stream, its key provider, and the
// plaintext the stream must read back as.
struct Fixture
{
  std::vector<BYTE> plaintext;
  std::vector<BYTE> image;
  std::shared_ptr<TestKeyProvider> provider;
};

Fixture MakeFixture(Algorithm algorithm, size_t size = 3000)
{
  Fixture fixture;
  fixture.plaintext = NtfsBrowserTests::PlaintextPattern(size);
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(algorithm);

  const TestEfsEntry user = TestUser();
  fixture.provider = std::make_shared<TestKeyProvider>();
  fixture.provider->Add(user.thumbprint, user.wrapped_fek,
                        NtfsBrowserTests::MakeFekBlob(algorithm, key));

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{kFirstStreamLcn, ClustersFor(size)}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              algorithm, key, fixture.plaintext),
                          .real_size = size});
  fixture.image = NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file);
  return fixture;
}

template <Strategy S>
struct Opened
{
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<FileRecord<S>> record;
};

// Opens the image and parses the root record. The provider is always set,
// null included, so that no test ever reaches the real certificate store.
// "mask" is nullopt for the default Mask::ALL; passing one exercises the
// same SetAttrMask() a caller narrowing to Mask::DATA would use. "options"
// defaults to strict; a test of a salvageable condition passes recover_errors.
template <Strategy S>
Opened<S> Open(std::vector<BYTE> image,
               std::shared_ptr<IEfsKeyProvider> provider,
               std::optional<Mask> mask = std::nullopt,
               const VolumeOptions& options = {})
{
  Opened<S> opened;
  opened.volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)),
      options);
  REQUIRE(opened.volume->IsVolumeOK());
  opened.volume->SetEfsKeyProvider(std::move(provider));

  opened.record = std::make_unique<FileRecord<S>>(*opened.volume);
  if (mask)
  {
    opened.record->SetAttrMask(*mask);
  }
  REQUIRE(opened.record->ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(opened.record->ParseAttrs());
  return opened;
}

// Reads up to "size" bytes at "offset". Nullopt if ReadData() failed.
template <Strategy S>
std::optional<std::vector<BYTE>> ReadAt(const AttrBase<S>& attr,
                                        ULONGLONG offset, size_t size)
{
  std::vector<BYTE> buffer(size, 0xCC);
  const std::optional<ULONGLONG> read = attr.ReadData(offset, buffer);
  if (!read)
  {
    return std::nullopt;
  }
  buffer.resize(static_cast<size_t>(*read));
  return buffer;
}

template <Strategy S>
const AttrBase<S>& OnlyData(const FileRecord<S>& record)
{
  const auto& data = record.getAttr(AttrType::DATA);
  REQUIRE(data.size() == 1);
  return *data.front();
}

std::string TakeLog() { return NtfsBrowserTests::TakeCapturedLog(); }

std::vector<BYTE> Slice(const std::vector<BYTE>& bytes, size_t offset,
                        size_t size)
{
  const size_t end = std::min(bytes.size(), offset + size);
  return {bytes.begin() + static_cast<std::ptrdiff_t>(offset),
          bytes.begin() + static_cast<std::ptrdiff_t>(end)};
}

void Patch32(std::vector<BYTE>& bytes, size_t offset, DWORD value)
{
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// Offsets, in a stream MakeEfsStream() builds for one user, of the fields the
// hostile-stream cases forge.
constexpr size_t kDdfOffsetField = 0x40;
constexpr size_t kDrfOffsetField = 0x44;
constexpr size_t kDdfCount = 0x54;
constexpr size_t kEntryLength = 0x58;
constexpr size_t kEntryCredential = 0x5C;
constexpr size_t kEntryFekLength = 0x60;
constexpr size_t kEntryFekOffset = 0x64;
constexpr size_t kCredentialHashField = 0x7C;
constexpr size_t kThumbprintOffset = 0xA4;
constexpr size_t kThumbprintSize = 0xA8;

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "An encrypted stream reads back as its plaintext, whatever the cipher",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  const NtfsBrowserTests::BackendGuard guard;

  for (const Algorithm algorithm : NtfsBrowserTests::kAllAlgorithms)
  {
    for (const CipherBackend backend :
         {CipherBackend::kCryptoPp, CipherBackend::kBCrypt})
    {
      if (!NtfsBrowser::Efs::SetCipherBackend(backend))
      {
        continue;
      }
#ifndef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
      if (algorithm == Algorithm::kDesx && backend == CipherBackend::kBCrypt)
      {
        // BCrypt has no DESX. With Crypto++ compiled in, MakeDecryptor()
        // falls back to it and this combination still round-trips; without
        // it there is no decryptor to fall back to at all, so this
        // combination cannot be exercised here.
        continue;
      }
#endif
      INFO("algorithm 0x" << std::hex << static_cast<DWORD>(algorithm)
                          << ", backend " << static_cast<int>(backend));

      Fixture fixture = MakeFixture(algorithm);
      Opened<S> opened = Open<S>(fixture.image, fixture.provider);
      const AttrBase<S>& data = OnlyData<S>(*opened.record);

      const auto whole = ReadAt<S>(data, 0, fixture.plaintext.size());
      REQUIRE(whole.has_value());
      CHECK(*whole == fixture.plaintext);

      // A second read: decrypting in place in a shared cache would garble it.
      const auto again = ReadAt<S>(data, 0, fixture.plaintext.size());
      REQUIRE(again.has_value());
      CHECK(*again == fixture.plaintext);
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "An unaligned read of an encrypted stream returns the plaintext slice",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  Fixture fixture = MakeFixture(Algorithm::kAes256);
  Opened<S> opened = Open<S>(fixture.image, fixture.provider);
  const AttrBase<S>& data = OnlyData<S>(*opened.record);

  for (const size_t offset :
       {0, 1, 511, 512, 513, 1023, 1024, 1025, 2047, 2999})
  {
    for (const size_t length : {1, 16, 511, 512, 513, 1500, 3000})
    {
      INFO("offset " << offset << ", length " << length);
      const auto read = ReadAt<S>(data, offset, length);
      REQUIRE(read.has_value());
      CHECK(*read == Slice(fixture.plaintext, offset, length));
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A hole inside an encrypted stream reads as zeros, not as decrypted data",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::kAes256);
  const TestEfsEntry user = TestUser();
  auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(user.thumbprint, user.wrapped_fek,
                NtfsBrowserTests::MakeFekBlob(Algorithm::kAes256, key));

  // Clusters 0-1 real, 2-3 a hole, 4 real: 5000 bytes.
  constexpr size_t kHoleStart = 2048;
  constexpr size_t kTailStart = 4096;
  const std::vector<BYTE> head = NtfsBrowserTests::PlaintextPattern(kHoleStart);
  const std::vector<BYTE> tail = NtfsBrowserTests::PlaintextPattern(904);

  std::vector<BYTE> cipher =
      NtfsBrowserTests::EfsEncrypt(Algorithm::kAes256, key, head, 0);
  const std::vector<BYTE> cipherTail =
      NtfsBrowserTests::EfsEncrypt(Algorithm::kAes256, key, tail, kTailStart);
  cipher.insert(cipher.end(), cipherTail.begin(), cipherTail.end());

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back(
      {.runs = {{kFirstStreamLcn, 2}, {{}, 2}, {kFirstStreamLcn + 8, 1}},
       .cluster_bytes = cipher,
       .real_size = kTailStart + tail.size()});

  std::vector<BYTE> expected = head;
  expected.resize(kTailStart, 0);
  expected.insert(expected.end(), tail.begin(), tail.end());

  Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);
  const auto read = ReadAt<S>(OnlyData<S>(*opened.record), 0, expected.size());
  REQUIRE(read.has_value());
  CHECK(*read == expected);
}

TEMPLATE_TEST_CASE_SIG(
    "Every $DATA stream flagged encrypted shares the record's key, named or "
    "not",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::kAes256);
  const TestEfsEntry user = TestUser();
  auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(user.thumbprint, user.wrapped_fek,
                NtfsBrowserTests::MakeFekBlob(Algorithm::kAes256, key));

  const std::vector<BYTE> unnamed = NtfsBrowserTests::PlaintextPattern(1800);
  const std::vector<BYTE> secret = NtfsBrowserTests::PlaintextPattern(700);
  const std::vector<BYTE> plain(300, 0x5A);

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{kFirstStreamLcn, 2}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              Algorithm::kAes256, key, unnamed),
                          .real_size = unnamed.size()});
  file.streams.push_back({.name = L"secret",
                          .runs = {{kFirstStreamLcn + 10, 1}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              Algorithm::kAes256, key, secret),
                          .real_size = secret.size()});
  file.streams.push_back({.name = L"plain",
                          .runs = {{kFirstStreamLcn + 20, 1}},
                          .cluster_bytes = plain,
                          .real_size = plain.size(),
                          .flagged_encrypted = false});

  Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);

  const AttrBase<S>* namedStream = opened.record->FindStream(L"secret");
  REQUIRE(namedStream != nullptr);
  CHECK(ReadAt<S>(*namedStream, 0, 700) == secret);

  const AttrBase<S>* plainStream = opened.record->FindStream(L"plain");
  REQUIRE(plainStream != nullptr);
  CHECK(ReadAt<S>(*plainStream, 0, 300) == plain);

  const AttrBase<S>* unnamedStream = opened.record->FindStream(L"");
  REQUIRE(unnamedStream != nullptr);
  CHECK(ReadAt<S>(*unnamedStream, 0, 1800) == unnamed);
}

TEMPLATE_TEST_CASE_SIG(
    "A stream nobody holds a key for reads as nullopt, and the parse survives",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  Fixture fixture = MakeFixture(Algorithm::kAes256);

  SECTION("no provider is installed")
  {
    (void)TakeLog();
    Opened<S> opened = Open<S>(fixture.image, nullptr);
    CHECK(opened.record->IsEncrypted());

    const AttrBase<S>& data = OnlyData<S>(*opened.record);
    CHECK_FALSE(ReadAt<S>(data, 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "Cannot decrypt the stream: no EFS key provider "
                              "is installed."));
  }

  SECTION("the provider holds another user's key")
  {
    auto stranger = std::make_shared<TestKeyProvider>();
    stranger->Add(
        NtfsBrowserTests::TestThumbprint(9), kWrappedFek,
        NtfsBrowserTests::MakeFekBlob(
            Algorithm::kAes256, NtfsBrowserTests::TestKey(Algorithm::kAes256)));

    (void)TakeLog();
    Opened<S> opened = Open<S>(fixture.image, stranger);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "no key provider holds a key for this file"));
  }

  SECTION("the unwrapped FEK is unusable")
  {
    auto broken = std::make_shared<TestKeyProvider>();
    std::vector<BYTE> blob(48, 0);
    Patch32(blob, 0, 32);
    Patch32(blob, 8, 0x1234);
    broken->Add(NtfsBrowserTests::TestThumbprint(1), kWrappedFek,
                std::move(blob));

    (void)TakeLog();
    Opened<S> opened = Open<S>(fixture.image, broken);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "the unwrapped FEK is unusable"));
  }

  SECTION("the record has no $EFS stream at all")
  {
    NtfsBrowserTests::FakeEncryptedFile file;
    file.streams.push_back({.runs = {{kFirstStreamLcn, 1}},
                            .cluster_bytes = std::vector<BYTE>(1024, 7),
                            .real_size = 1000});

    (void)TakeLog();
    Opened<S> opened =
        Open<S>(NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file),
                fixture.provider);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
    CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                              "the record has no usable $EFS stream"));
  }
}

TEMPLATE_TEST_CASE_SIG("The key can come from any entry of the $EFS stream",
                       "[efs]", ((Strategy S), S), Strategy::NO_CACHE,
                       Strategy::FULL_CACHE)
{
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::kAes128);
  const std::vector<BYTE> plaintext = NtfsBrowserTests::PlaintextPattern(900);

  const std::vector<BYTE> otherWrapped(32, 0x11);
  const std::vector<BYTE> recoveryWrapped(32, 0x22);
  const std::array<TestEfsEntry, 2> users{TestUser(1, otherWrapped),
                                          TestUser(2, kWrappedFek)};
  const std::array<TestEfsEntry, 1> recovery{TestUser(3, recoveryWrapped)};

  SECTION("a later DDF entry")
  {
    auto provider = std::make_shared<TestKeyProvider>();
    provider->Add(users[1].thumbprint, users[1].wrapped_fek,
                  NtfsBrowserTests::MakeFekBlob(Algorithm::kAes128, key));

    NtfsBrowserTests::FakeEncryptedFile file;
    file.efs_stream = NtfsBrowserTests::MakeEfsStream(users);
    file.streams.push_back({.runs = {{kFirstStreamLcn, 1}},
                            .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                                Algorithm::kAes128, key, plaintext),
                            .real_size = plaintext.size()});
    Opened<S> opened = Open<S>(
        NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);
    CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, 900) == plaintext);
  }

  SECTION("a recovery agent's entry")
  {
    auto provider = std::make_shared<TestKeyProvider>();
    provider->Add(recovery[0].thumbprint, recovery[0].wrapped_fek,
                  NtfsBrowserTests::MakeFekBlob(Algorithm::kAes128, key));

    NtfsBrowserTests::FakeEncryptedFile file;
    file.efs_stream = NtfsBrowserTests::MakeEfsStream(users, recovery);
    file.streams.push_back({.runs = {{kFirstStreamLcn, 1}},
                            .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                                Algorithm::kAes128, key, plaintext),
                            .real_size = plaintext.size()});
    Opened<S> opened = Open<S>(
        NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);
    CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, 900) == plaintext);
  }
}

TEMPLATE_TEST_CASE_SIG("A resident $EFS stream works like a non-resident one",
                       "[efs]", ((Strategy S), S), Strategy::NO_CACHE,
                       Strategy::FULL_CACHE)
{
  const std::vector<BYTE> key = NtfsBrowserTests::TestKey(Algorithm::kAes256);
  const std::vector<BYTE> plaintext = NtfsBrowserTests::PlaintextPattern(900);
  const TestEfsEntry user = TestUser();
  auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(user.thumbprint, user.wrapped_fek,
                NtfsBrowserTests::MakeFekBlob(Algorithm::kAes256, key));

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_resident = true;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{kFirstStreamLcn, 1}},
                          .cluster_bytes = NtfsBrowserTests::EfsEncrypt(
                              Algorithm::kAes256, key, plaintext),
                          .real_size = plaintext.size()});
  Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file), provider);
  CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, 900) == plaintext);
}

TEMPLATE_TEST_CASE_SIG(
    "SetAttrMask(Mask::DATA) still pulls in the $EFS stream it needs", "[efs]",
    ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  // The exact mask ntfsdump/ntfsundel narrow to before reading file data.
  Fixture fixture = MakeFixture(Algorithm::kAes256);
  Opened<S> opened = Open<S>(fixture.image, fixture.provider, Mask::DATA);
  CHECK(ReadAt<S>(OnlyData<S>(*opened.record), 0, fixture.plaintext.size()) ==
        fixture.plaintext);
}

TEMPLATE_TEST_CASE_SIG(
    "A $DATA stream flagged both compressed and encrypted is left "
    "undecrypted when recovering",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  const TestEfsEntry user = TestUser();
  auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NtfsBrowserTests::MakeFekBlob(
          Algorithm::kAes256, NtfsBrowserTests::TestKey(Algorithm::kAes256)));

  const std::vector<BYTE> onDisk(NtfsBrowserTests::kFakeClusterSize, 0x42);

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{kFirstStreamLcn, 1}},
                          .cluster_bytes = onDisk,
                          .real_size = onDisk.size(),
                          .flagged_encrypted = true,
                          .flagged_compressed = true});

  (void)TakeLog();
  Opened<S> opened =
      Open<S>(NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file),
              provider, std::nullopt, VolumeOptions{.recover_errors = true});

  // Never decrypted: the bytes come back exactly as they sit on disk.
  const auto read = ReadAt<S>(OnlyData<S>(*opened.record), 0, onDisk.size());
  REQUIRE(read.has_value());
  CHECK(*read == onDisk);
  CHECK_THAT(TakeLog(), Catch::Matchers::ContainsSubstring(
                            "flagged both compressed and encrypted"));
}

TEMPLATE_TEST_CASE_SIG(
    "A $DATA stream flagged both compressed and encrypted rejects the "
    "record by default",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  const TestEfsEntry user = TestUser();
  auto provider = std::make_shared<TestKeyProvider>();
  provider->Add(
      user.thumbprint, user.wrapped_fek,
      NtfsBrowserTests::MakeFekBlob(
          Algorithm::kAes256, NtfsBrowserTests::TestKey(Algorithm::kAes256)));

  const std::vector<BYTE> onDisk(NtfsBrowserTests::kFakeClusterSize, 0x42);

  NtfsBrowserTests::FakeEncryptedFile file;
  file.efs_stream = NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  file.streams.push_back({.runs = {{kFirstStreamLcn, 1}},
                          .cluster_bytes = onDisk,
                          .real_size = onDisk.size(),
                          .flagged_encrypted = true,
                          .flagged_compressed = true});

  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file)));
  REQUIRE(volume->IsVolumeOK());
  volume->SetEfsKeyProvider(provider);

  FileRecord<S> record(*volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::DATA).empty());
}

TEMPLATE_TEST_CASE_SIG("An encrypted directory parses and lists its entries",
                       "[efs]", ((Strategy S), S), Strategy::NO_CACHE,
                       Strategy::FULL_CACHE)
{
  Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedDirectory(), nullptr);
  CHECK(opened.record->IsEncrypted());
  CHECK(opened.record->IsDirectory());

  std::vector<std::wstring> names;
  opened.record->TraverseSubEntries(
      [](const IndexEntry& entry, void* context)
      {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            entry.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 2);
  CHECK(names[0] == NtfsBrowserTests::kEncryptedDirectoryNames[0]);
  CHECK(names[1] == NtfsBrowserTests::kEncryptedDirectoryNames[1]);
}

#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
// This test's whole premise is a compressed $INDEX_ALLOCATION: with
// decompression not compiled in, that attribute is rejected on sight
// (see attr-non-resident.cpp), so the record never parses at all. There is
// no meaningful compressed-and-encrypted-directory case left to check then,
// the same way attr-compression-tests.cpp is excluded outright.
TEMPLATE_TEST_CASE_SIG(
    "A compressed $INDEX_ALLOCATION still lists under an encrypted directory",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  Opened<S> opened = Open<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCompressedEncryptedDirectory(),
      nullptr);
  CHECK(opened.record->IsEncrypted());
  CHECK(opened.record->IsCompressed());
  CHECK(opened.record->IsDirectory());

  std::vector<std::wstring> names;
  opened.record->TraverseSubEntries(
      [](const IndexEntry& entry, void* context)
      {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            entry.GetFilename());
      },
      &names);

  REQUIRE(names.size() == 2);
  CHECK(names[0] == NtfsBrowserTests::kEncryptedDirectoryNames[0]);
  CHECK(names[1] == NtfsBrowserTests::kEncryptedDirectoryNames[1]);
}
#endif

TEMPLATE_TEST_CASE_SIG(
    "A hostile $EFS stream never crashes the parse and never yields a key",
    "[efs]", ((Strategy S), S), Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  const TestEfsEntry user = TestUser();
  const std::vector<BYTE> valid =
      NtfsBrowserTests::MakeEfsStream(std::span(&user, 1));
  REQUIRE(NtfsBrowser::Efs::ParseEfsStream(valid).has_value());

  std::vector<std::vector<BYTE>> hostile;
  const auto forge = [&](size_t offset, DWORD value)
  {
    std::vector<BYTE> copy = valid;
    Patch32(copy, offset, value);
    hostile.push_back(std::move(copy));
  };
  forge(kDdfOffsetField, 0xFFFFFFF0);
  forge(kDdfOffsetField, static_cast<DWORD>(valid.size()));
  forge(kDrfOffsetField, 0x7FFFFFFF);
  forge(kDdfCount, 0xFFFFFFFF);
  forge(kDdfCount, 65);
  forge(kDdfCount, 2);
  forge(kEntryLength, 0);
  forge(kEntryLength, 0xFFFFFFFF);
  forge(kEntryCredential, 0xFFFFFF00);
  forge(kEntryFekLength, 0xFFFFFFFF);
  forge(kEntryFekLength, 0);
  forge(kEntryFekOffset, 0xFFFFFFFF);
  forge(kCredentialHashField, 0xFFFFFFFF);
  forge(kThumbprintOffset, 0xFFFFFFFF);
  forge(kThumbprintSize, 21);
  forge(kThumbprintSize, 0xFFFFFFFF);
  for (const size_t length : {size_t{0}, size_t{3}, size_t{0x47}, size_t{0x54},
                              size_t{0x58}, valid.size() - 1})
  {
    hostile.emplace_back(valid.begin(), valid.begin() + length);
  }
  hostile.emplace_back(700, 0xFF);

  Fixture fixture = MakeFixture(Algorithm::kAes256, 1000);
  for (const std::vector<BYTE>& stream : hostile)
  {
    INFO("stream of " << stream.size() << " bytes");
    CHECK_FALSE(NtfsBrowser::Efs::ParseEfsStream(stream).has_value());

    NtfsBrowserTests::FakeEncryptedFile file;
    file.efs_stream = stream;
    file.streams.push_back({.runs = {{kFirstStreamLcn, 1}},
                            .cluster_bytes = std::vector<BYTE>(1024, 7),
                            .real_size = 1000});
    if (stream.empty())
    {
      continue;
    }
    Opened<S> opened =
        Open<S>(NtfsBrowserTests::BuildFakeNtfsImageWithEncryptedFile(file),
                fixture.provider);
    CHECK_FALSE(ReadAt<S>(OnlyData<S>(*opened.record), 0, 100).has_value());
  }
}

TEST_CASE("A $EFS stream with random damage never crashes the parser", "[efs]")
{
  const std::array<TestEfsEntry, 2> users{TestUser(1), TestUser(2)};
  const std::vector<BYTE> valid = NtfsBrowserTests::MakeEfsStream(users, users);

  std::mt19937 random(20260921);
  for (int round = 0; round < 5000; ++round)
  {
    std::vector<BYTE> damaged = valid;
    const int flips = 1 + static_cast<int>(random() % 4);
    for (int i = 0; i < flips; ++i)
    {
      damaged[random() % damaged.size()] = static_cast<BYTE>(random());
    }
    if ((random() % 4) == 0)
    {
      damaged.resize(random() % damaged.size());
    }
    (void)NtfsBrowser::Efs::ParseEfsStream(damaged);
  }
  SUCCEED();
}

TEST_CASE("The $EFS parser reads the users out of a well-formed stream",
          "[efs]")
{
  const std::array<TestEfsEntry, 2> users{TestUser(1, std::vector<BYTE>(8, 1)),
                                          TestUser(2, std::vector<BYTE>(9, 2))};
  const std::array<TestEfsEntry, 1> recovery{
      TestUser(3, std::vector<BYTE>(10, 3))};

  const auto parsed = NtfsBrowser::Efs::ParseEfsStream(
      NtfsBrowserTests::MakeEfsStream(users, recovery));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->size() == 3);

  for (size_t i = 0; i < 2; ++i)
  {
    CHECK(std::equal((*parsed)[i].thumbprint.begin(),
                     (*parsed)[i].thumbprint.end(),
                     users[i].thumbprint.begin()));
    CHECK((*parsed)[i].wrapped_fek == users[i].wrapped_fek);
  }
  CHECK((*parsed)[2].wrapped_fek == recovery[0].wrapped_fek);

  CHECK(
      NtfsBrowser::Efs::ParseEfsStream(NtfsBrowserTests::MakeEfsStream({}, {}))
          ->empty());
}

TEST_CASE("A FEK blob is checked against its algorithm", "[efs]")
{
  for (const Algorithm algorithm : NtfsBrowserTests::kAllAlgorithms)
  {
    const std::vector<BYTE> key = NtfsBrowserTests::TestKey(algorithm);
    const std::vector<BYTE> blob =
        NtfsBrowserTests::MakeFekBlob(algorithm, key);

    const std::optional<Fek> fek = Fek::Parse(blob);
    REQUIRE(fek.has_value());
    CHECK(fek->GetAlgorithm() == algorithm);
    CHECK(std::equal(fek->GetKey().begin(), fek->GetKey().end(), key.begin(),
                     key.end()));
  }

  const std::vector<BYTE> good = NtfsBrowserTests::MakeFekBlob(
      Algorithm::kAes256, NtfsBrowserTests::TestKey(Algorithm::kAes256));

  CHECK_FALSE(Fek::Parse({}).has_value());
  CHECK_FALSE(Fek::Parse(std::span(good).first(15)).has_value());
  CHECK_FALSE(Fek::Parse(std::span(good).first(good.size() - 1)).has_value());

  std::vector<BYTE> unknown = good;
  Patch32(unknown, 8, 0x6611);
  CHECK_FALSE(Fek::Parse(unknown).has_value());

  std::vector<BYTE> wrongLength = good;
  Patch32(wrongLength, 0, 16);
  CHECK_FALSE(Fek::Parse(wrongLength).has_value());

  std::vector<BYTE> hugeLength = good;
  Patch32(hugeLength, 0, 0xFFFFFFFF);
  CHECK_FALSE(Fek::Parse(hugeLength).has_value());
}

TEST_CASE("Secure zero wipes what it is given", "[efs]")
{
  std::vector<BYTE> secret(64, 0xEE);
  NtfsBrowser::Efs::SecureZero(secret);
  CHECK(std::all_of(secret.begin(), secret.end(),
                    [](BYTE byte) { return byte == 0; }));
}

TEST_CASE("The cipher backend is selectable, and Crypto++ is the default",
          "[efs]")
{
  const NtfsBrowserTests::BackendGuard guard;

#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
  CHECK(NtfsBrowser::Efs::SetCipherBackend(CipherBackend::kCryptoPp));
  CHECK(NtfsBrowser::Efs::GetCipherBackend() == CipherBackend::kCryptoPp);
#else
  // Crypto++ is not compiled in: the file only builds at all because BCrypt
  // is, so that is the default instead (mirrors efs.cpp's g_backend default).
  CHECK_FALSE(NtfsBrowser::Efs::SetCipherBackend(CipherBackend::kCryptoPp));
  CHECK(NtfsBrowser::Efs::GetCipherBackend() == CipherBackend::kBCrypt);
#endif

#if defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
  CHECK(NtfsBrowser::Efs::SetCipherBackend(CipherBackend::kBCrypt));
  CHECK(NtfsBrowser::Efs::GetCipherBackend() == CipherBackend::kBCrypt);
#else
  CHECK_FALSE(NtfsBrowser::Efs::SetCipherBackend(CipherBackend::kBCrypt));
  CHECK(NtfsBrowser::Efs::GetCipherBackend() == CipherBackend::kCryptoPp);
#endif
}

namespace
{

// One published block-cipher known-answer: the first block of the sector's
// ciphertext, and the plaintext block it decrypts to under a zero-based IV.
struct KnownAnswer
{
  Algorithm algorithm;
  std::vector<BYTE> key;
  std::vector<BYTE> ciphertext_block;
  std::vector<BYTE> plaintext_block;
};

std::vector<BYTE> Bytes(std::initializer_list<int> values)
{
  return {values.begin(), values.end()};
}

std::vector<BYTE> Counting(size_t length)
{
  std::vector<BYTE> bytes(length);
  for (size_t i = 0; i < length; ++i)
  {
    bytes[i] = static_cast<BYTE>(i);
  }
  return bytes;
}

// The first IV bytes of sector 0, as read off a real EFS file.
const std::vector<BYTE> kSectorZeroIv =
    Bytes({0x12, 0x13, 0x16, 0xe9, 0x7b, 0x65, 0x16, 0x58, 0x61, 0x89, 0x91,
           0x44, 0xbe, 0xad, 0x89, 0x19});

}  // namespace

TEST_CASE("Sector decryption matches the published block-cipher vectors",
          "[efs]")
{
  const NtfsBrowserTests::BackendGuard guard;

  // FIPS-197 appendix C for AES. For 3DES, the FIPS 81 vector: with all three
  // keys equal, 3DES is single DES.
  const std::vector<KnownAnswer> answers{
      {Algorithm::kAes128, Counting(16),
       Bytes({0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7,
              0x80, 0x70, 0xb4, 0xc5, 0x5a}),
       Bytes({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
              0xbb, 0xcc, 0xdd, 0xee, 0xff})},
      {Algorithm::kAes192, Counting(24),
       Bytes({0xdd, 0xa9, 0x7c, 0xa4, 0x86, 0x4c, 0xdf, 0xe0, 0x6e, 0xaf, 0x70,
              0xa0, 0xec, 0x0d, 0x71, 0x91}),
       Bytes({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
              0xbb, 0xcc, 0xdd, 0xee, 0xff})},
      {Algorithm::kAes256, Counting(32),
       Bytes({0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49,
              0x90, 0x4b, 0x49, 0x60, 0x89}),
       Bytes({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
              0xbb, 0xcc, 0xdd, 0xee, 0xff})},
      {Algorithm::k3Des,
       Bytes({0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
              0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
              0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}),
       Bytes({0x3f, 0xa4, 0x0e, 0x8a, 0x98, 0x4d, 0x48, 0x15}),
       Bytes({0x4e, 0x6f, 0x77, 0x20, 0x69, 0x73, 0x20, 0x74})}};

  for (const KnownAnswer& answer : answers)
  {
    for (const CipherBackend backend :
         {CipherBackend::kCryptoPp, CipherBackend::kBCrypt})
    {
      if (!NtfsBrowser::Efs::SetCipherBackend(backend))
      {
        continue;
      }
      INFO("algorithm 0x" << std::hex << static_cast<DWORD>(answer.algorithm)
                          << ", backend " << static_cast<int>(backend));

      // CBC: the first block decrypts to (block cipher output) xor IV. So a
      // ciphertext block that is the published one must give plaintext xor IV.
      const std::vector<BYTE> blob =
          NtfsBrowserTests::MakeFekBlob(answer.algorithm, answer.key);
      const std::optional<Fek> fek = Fek::Parse(blob);
      REQUIRE(fek.has_value());

#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
      auto decryptor = NtfsBrowser::Efs::MakeCryptoPpDecryptor(*fek);
#else
      std::unique_ptr<NtfsBrowser::Efs::SectorDecryptor> decryptor;
#endif
#if defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
      if (backend == CipherBackend::kBCrypt)
      {
        decryptor = NtfsBrowser::Efs::MakeBCryptDecryptor(*fek);
      }
#endif
      REQUIRE(decryptor != nullptr);

      std::vector<BYTE> sector(NtfsBrowser::Efs::kSectorSize, 0);
      std::copy(answer.ciphertext_block.begin(), answer.ciphertext_block.end(),
                sector.begin());
      REQUIRE(decryptor->DecryptSector(0, sector));

      for (size_t i = 0; i < answer.plaintext_block.size(); ++i)
      {
        CHECK(sector[i] ==
              static_cast<BYTE>(answer.plaintext_block[i] ^ kSectorZeroIv[i]));
      }
    }
  }
}
