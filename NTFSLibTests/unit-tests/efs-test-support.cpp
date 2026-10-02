#include "efs-test-support.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <random>
#include <utility>

#include <cryptopp/aes.h>
#include <cryptopp/des.h>
#include <gsl/narrow>
// Silences the weak-algorithm notice: MD5 is what the DESX key expansion uses.
#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
#include <cryptopp/md5.h>
#include <cryptopp/modes.h>

namespace NtfsBrowserTests
{

namespace
{
// EFS encrypts one 512-byte sector at a time.
constexpr size_t kSector = 512;

// Cipher block sizes, in bytes.
constexpr size_t kAesBlockSize = 16;
constexpr size_t kDesBlockSize = 8;

// Size of an MD5 digest, and of one DESX salt: 11 characters and their NUL.
constexpr size_t kMd5DigestSize = 16;
constexpr size_t kDesxSaltSize = 12;

// Key lengths, in bytes. DESX takes 128 bits, which the cipher expands.
constexpr size_t kAes128KeySize = 16;
constexpr size_t kAes192KeySize = 24;
constexpr size_t kAes256KeySize = 32;
constexpr size_t k3DesKeySize = 24;
constexpr size_t kDesxKeySize = 16;

// How TestKey() fills a key byte: (index * step) + seed.
constexpr size_t kKeyByteStep = 7;
constexpr size_t kKeyByteSeed = 0x21;

// Size of the FEK blob header: key length, entropy, algorithm, reserved.
constexpr size_t kFekHeaderSize = 16;

// How TestThumbprint() steps from one byte to the next.
constexpr size_t kThumbprintByteStep = 13;

// Seed of the plaintext pattern's engine: the 64-bit golden ratio constant.
constexpr ULONGLONG kPatternSeed = 0x9E3779B97F4A7C15ULL;

// Shift that keeps the top byte of a 64-bit engine word.
constexpr unsigned kTopByteShift = 56;

// Field offsets of a synthetic $EFS stream, entry, credential and hash record.
constexpr size_t kStreamVersionField = 0x08;
constexpr size_t kDdfOffsetField = 0x40;
constexpr size_t kDrfOffsetField = 0x44;
constexpr size_t kEntryFekLengthField = 0x08;
constexpr size_t kEntryFekOffsetField = 0x0C;
constexpr size_t kCredentialHashField = 0x10;

// Offset, in the credential, of its SID type DWORD, and the value it holds.
constexpr size_t kCredentialSidTypeField = 0x08;
constexpr DWORD kSidTypeUser = 3;

// Offsets, in a FEK blob, of the key length, entropy length and algorithm.
constexpr size_t kFekKeyLengthField = 0;
constexpr size_t kFekEntropyLengthField = 4;
constexpr size_t kFekAlgorithmField = 8;

// The two IV words, as read off a real AES-256 EFS file. Repeated here on
// purpose: the tests must not share the library's copy.
constexpr ULONGLONG kIvWord0 = 0x5816657be9161312ULL;
constexpr ULONGLONG kIvWord1 = 0x1989adbe44918961ULL;

// The IV word of the DES family: 3DES and DESX. It is a single 8-byte word,
// since the block is 8 bytes. Taken from ntfs-3g, not from the AES constants.
constexpr ULONGLONG kDesIvWord = 0x169119629891ad13ULL;

// Little-endian CBC IV of the sector at "offset": two words for the 16-byte
// AES block, one for the 8-byte DES block.
std::vector<BYTE> SectorIv(ULONGLONG offset, size_t blockSize)
{
  std::vector<BYTE> iv(blockSize, 0);
  if (blockSize == kAesBlockSize)
  {
    const ULONGLONG w0 = kIvWord0 + offset;
    const ULONGLONG w1 = kIvWord1 + offset;
    std::memcpy(iv.data(), &w0, sizeof(w0));
    std::memcpy(iv.data() + sizeof(w0), &w1, sizeof(w1));
  }
  else
  {
    const ULONGLONG w0 = kDesIvWord + offset;
    std::memcpy(iv.data(), &w0, sizeof(w0));
  }
  return iv;
}

template <class BlockCipher>
std::vector<BYTE> EncryptWith(std::span<const BYTE> key,
                              std::span<const BYTE> plaintext,
                              ULONGLONG streamOffset)
{
  std::vector<BYTE> data(plaintext.begin(), plaintext.end());
  data.resize(((data.size() + kSector - 1) / kSector) * kSector, 0);

  typename BlockCipher::Encryption cipher;
  cipher.SetKey(key.data(), key.size());
  for (size_t done = 0; done < data.size(); done += kSector)
  {
    const auto iv = SectorIv(streamOffset + done, BlockCipher::BLOCKSIZE);
    CryptoPP::CBC_Mode_ExternalCipher::Encryption cbc(cipher, iv.data());
    cbc.ProcessData(data.data() + done, data.data() + done, kSector);
  }
  return data;
}

// Encrypts as EFS does with DESX, written out by hand: a 128-bit FEK is
// expanded with MD5 into a DES key and two whitening keys, and each block is
// out_whitening ^ DES(block ^ prev ^ in_whitening), chained per sector.
std::vector<BYTE> EncryptDesx(std::span<const BYTE> key,
                              std::span<const BYTE> plaintext,
                              ULONGLONG streamOffset)
{
  // The salts include their terminating NUL: 12 bytes each.
  constexpr std::array<char, kDesxSaltSize> kSalt1{"Dan Simon  "};
  constexpr std::array<char, kDesxSaltSize> kSalt2{"Scott Field"};

  const auto digest = [&key](const std::array<char, kDesxSaltSize>& salt)
  {
    std::array<BYTE, kMd5DigestSize> md{};
    CryptoPP::Weak::MD5 hash;
    hash.Update(key.data(), key.size());
    hash.Update(reinterpret_cast<const BYTE*>(salt.data()), salt.size());
    hash.Final(md.data());
    return md;
  };

  const std::array<BYTE, kMd5DigestSize> md1 = digest(kSalt1);
  std::array<DWORD, 4> words1{};
  std::memcpy(words1.data(), md1.data(), md1.size());
  const std::array<DWORD, 2> desKeyWords{words1[0] ^ words1[1],
                                         words1[2] ^ words1[3]};
  std::array<BYTE, kDesBlockSize> desKey{};
  std::memcpy(desKey.data(), desKeyWords.data(), desKey.size());

  const std::array<BYTE, kMd5DigestSize> md2 = digest(kSalt2);
  ULONGLONG outWhitening = 0;
  ULONGLONG inWhitening = 0;
  std::memcpy(&outWhitening, md2.data(), sizeof(outWhitening));
  std::memcpy(&inWhitening, md2.data() + sizeof(outWhitening),
              sizeof(inWhitening));

  CryptoPP::DES::Encryption des;
  des.SetKey(desKey.data(), desKey.size());

  std::vector<BYTE> data(plaintext.begin(), plaintext.end());
  data.resize(((data.size() + kSector - 1) / kSector) * kSector, 0);
  for (size_t done = 0; done < data.size(); done += kSector)
  {
    const auto iv = SectorIv(streamOffset + done, kDesBlockSize);
    ULONGLONG prev = 0;
    std::memcpy(&prev, iv.data(), sizeof(prev));
    for (size_t block = 0; block < kSector; block += kDesBlockSize)
    {
      BYTE* bytes = data.data() + done + block;
      ULONGLONG value = 0;
      std::memcpy(&value, bytes, sizeof(value));
      value ^= prev ^ inWhitening;
      std::memcpy(bytes, &value, sizeof(value));
      des.ProcessBlock(bytes);
      std::memcpy(&value, bytes, sizeof(value));
      value ^= outWhitening;
      std::memcpy(bytes, &value, sizeof(value));
      prev = value;
    }
  }
  return data;
}

void Put32(std::vector<BYTE>& out, size_t offset, DWORD value)
{
  std::memcpy(out.data() + offset, &value, sizeof(value));
}

// Appends one entry, in the layout of a real one, to "out".
void AppendEntry(std::vector<BYTE>& out, const TestEfsEntry& user)
{
  // The credential: a 28-byte SID, then the certificate hash record. Five
  // DWORDs, then the 20-byte thumbprint.
  constexpr size_t kSidOffset = 0x1C;
  constexpr size_t kHashOffset = 0x38;
  constexpr size_t kHashHeader = 0x14;
  constexpr size_t kCredentialSize =
      kHashOffset + kHashHeader + kThumbprintSize;
  constexpr size_t kEntryHeader = 0x14;

  const size_t base = out.size();
  const size_t fekOffset = kEntryHeader + kCredentialSize;
  out.resize(base + fekOffset + user.wrapped_fek.size(), 0);

  Put32(out, base + 0x00, gsl::narrow<DWORD>(out.size() - base));
  Put32(out, base + 0x04, kEntryHeader);
  Put32(out, base + kEntryFekLengthField,
        gsl::narrow<DWORD>(user.wrapped_fek.size()));
  Put32(out, base + kEntryFekOffsetField, gsl::narrow<DWORD>(fekOffset));

  const size_t cred = base + kEntryHeader;
  Put32(out, cred + 0x00, static_cast<DWORD>(kCredentialSize));
  Put32(out, cred + 0x04, kSidOffset);
  Put32(out, cred + kCredentialSidTypeField, kSidTypeUser);
  Put32(out, cred + kCredentialHashField, kHashOffset);

  const size_t hash = cred + kHashOffset;
  Put32(out, hash + 0x00, kHashHeader);
  Put32(out, hash + 0x04, gsl::narrow<DWORD>(kThumbprintSize));
  std::memcpy(out.data() + hash + kHashHeader, user.thumbprint.data(),
              kThumbprintSize);
  std::memcpy(out.data() + base + fekOffset, user.wrapped_fek.data(),
              user.wrapped_fek.size());
}

// Appends a field (count, then entries) and returns its offset.
size_t AppendField(std::vector<BYTE>& out, std::span<const TestEfsEntry> users)
{
  const size_t offset = out.size();
  out.resize(offset + sizeof(DWORD), 0);
  Put32(out, offset, gsl::narrow<DWORD>(users.size()));
  for (const TestEfsEntry& user : users)
  {
    AppendEntry(out, user);
  }
  return offset;
}
}  // namespace

std::vector<BYTE> TestKey(Algorithm algorithm)
{
  size_t length = 0;
  switch (algorithm)
  {
    case Algorithm::kAes128:
      length = kAes128KeySize;
      break;
    case Algorithm::kAes192:
      length = kAes192KeySize;
      break;
    case Algorithm::k3Des:
      length = k3DesKeySize;
      break;
    case Algorithm::kDesx:
      length = kDesxKeySize;
      break;
    case Algorithm::kAes256:
      length = kAes256KeySize;
      break;
  }

  std::vector<BYTE> key(length);
  for (size_t i = 0; i < length; ++i)
  {
    key[i] = gsl::narrow<BYTE>((i * kKeyByteStep) + kKeyByteSeed);
  }
  return key;
}

std::vector<BYTE> MakeFekBlob(Algorithm algorithm, std::span<const BYTE> key)
{
  std::vector<BYTE> blob(kFekHeaderSize + key.size(), 0);
  Put32(blob, kFekKeyLengthField, gsl::narrow<DWORD>(key.size()));
  Put32(blob, kFekEntropyLengthField, gsl::narrow<DWORD>(key.size()));
  Put32(blob, kFekAlgorithmField, static_cast<DWORD>(algorithm));
  std::ranges::copy(key, blob.begin() + kFekHeaderSize);
  return blob;
}

std::vector<BYTE> EfsEncrypt(Algorithm algorithm, std::span<const BYTE> key,
                             std::span<const BYTE> plaintext,
                             ULONGLONG streamOffset)
{
  switch (algorithm)
  {
    case Algorithm::kAes128:
    case Algorithm::kAes192:
    case Algorithm::kAes256:
      return EncryptWith<CryptoPP::AES>(key, plaintext, streamOffset);
    case Algorithm::k3Des:
      return EncryptWith<CryptoPP::DES_EDE3>(key, plaintext, streamOffset);
    case Algorithm::kDesx:
      return EncryptDesx(key, plaintext, streamOffset);
  }
  return {};
}

std::vector<BYTE> PlaintextPattern(size_t size)
{
  std::vector<BYTE> bytes(size);
  std::mt19937_64 engine(kPatternSeed);
  for (BYTE& byte : bytes)
  {
    byte = static_cast<BYTE>(engine() >> kTopByteShift);
  }
  return bytes;
}

std::array<BYTE, kThumbprintSize> TestThumbprint(BYTE seed)
{
  std::array<BYTE, kThumbprintSize> thumbprint{};
  for (size_t i = 0; i < thumbprint.size(); ++i)
  {
    thumbprint[i] = static_cast<BYTE>(seed + (i * kThumbprintByteStep));
  }
  return thumbprint;
}

std::vector<BYTE> MakeEfsStream(std::span<const TestEfsEntry> users,
                                std::span<const TestEfsEntry> recovery)
{
  // The header ends where the first field starts, as in a real stream.
  constexpr size_t kHeaderSize = 0x54;
  std::vector<BYTE> out(kHeaderSize, 0);
  Put32(out, kStreamVersionField, 2);

  if (!users.empty())
  {
    Put32(out, kDdfOffsetField, gsl::narrow<DWORD>(AppendField(out, users)));
  }
  if (!recovery.empty())
  {
    Put32(out, kDrfOffsetField, gsl::narrow<DWORD>(AppendField(out, recovery)));
  }
  Put32(out, 0x00, gsl::narrow<DWORD>(out.size()));
  return out;
}

void TestKeyProvider::Add(const std::array<BYTE, kThumbprintSize>& thumbprint,
                          std::span<const BYTE> wrappedFek,
                          std::vector<BYTE> blob)
{
  known_.push_back(
      {thumbprint, {wrappedFek.begin(), wrappedFek.end()}, std::move(blob)});
}

std::optional<std::vector<BYTE>>
    TestKeyProvider::UnwrapFek(std::span<const BYTE> thumbprint,
                               std::span<const BYTE> wrappedFek) const
{
  for (const Known& known : known_)
  {
    if (std::ranges::equal(thumbprint, known.thumbprint) &&
        std::ranges::equal(wrappedFek, known.wrapped_fek))
    {
      return known.blob;
    }
  }
  return std::nullopt;
}

}  // namespace NtfsBrowserTests
