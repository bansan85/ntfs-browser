#include "efs-test-support.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include <cryptopp/aes.h>
#include <cryptopp/des.h>
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
  if (blockSize == 16)
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
  constexpr std::array<char, 12> kSalt1{"Dan Simon  "};
  constexpr std::array<char, 12> kSalt2{"Scott Field"};

  const auto digest = [&key](const std::array<char, 12>& salt)
  {
    std::array<BYTE, 16> md{};
    CryptoPP::Weak::MD5 hash;
    hash.Update(key.data(), key.size());
    hash.Update(reinterpret_cast<const BYTE*>(salt.data()), salt.size());
    hash.Final(md.data());
    return md;
  };

  const std::array<BYTE, 16> md1 = digest(kSalt1);
  std::array<DWORD, 4> words1{};
  std::memcpy(words1.data(), md1.data(), md1.size());
  const std::array<DWORD, 2> desKeyWords{words1[0] ^ words1[1],
                                         words1[2] ^ words1[3]};
  std::array<BYTE, 8> desKey{};
  std::memcpy(desKey.data(), desKeyWords.data(), desKey.size());

  const std::array<BYTE, 16> md2 = digest(kSalt2);
  ULONGLONG outWhitening = 0;
  ULONGLONG inWhitening = 0;
  std::memcpy(&outWhitening, md2.data(), sizeof(outWhitening));
  std::memcpy(&inWhitening, md2.data() + 8, sizeof(inWhitening));

  CryptoPP::DES::Encryption des;
  des.SetKey(desKey.data(), desKey.size());

  std::vector<BYTE> data(plaintext.begin(), plaintext.end());
  data.resize(((data.size() + kSector - 1) / kSector) * kSector, 0);
  for (size_t done = 0; done < data.size(); done += kSector)
  {
    const auto iv = SectorIv(streamOffset + done, 8);
    ULONGLONG prev = 0;
    std::memcpy(&prev, iv.data(), sizeof(prev));
    for (size_t block = 0; block < kSector; block += 8)
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
  constexpr size_t kCredentialSize = kHashOffset + kHashHeader + 20;
  constexpr size_t kEntryHeader = 0x14;

  const size_t base = out.size();
  const size_t fekOffset = kEntryHeader + kCredentialSize;
  out.resize(base + fekOffset + user.wrapped_fek.size(), 0);

  Put32(out, base + 0x00, static_cast<DWORD>(out.size() - base));
  Put32(out, base + 0x04, kEntryHeader);
  Put32(out, base + 0x08, static_cast<DWORD>(user.wrapped_fek.size()));
  Put32(out, base + 0x0C, static_cast<DWORD>(fekOffset));

  const size_t cred = base + kEntryHeader;
  Put32(out, cred + 0x00, static_cast<DWORD>(kCredentialSize));
  Put32(out, cred + 0x04, kSidOffset);
  Put32(out, cred + 0x08, 3);
  Put32(out, cred + 0x10, kHashOffset);

  const size_t hash = cred + kHashOffset;
  Put32(out, hash + 0x00, kHashHeader);
  Put32(out, hash + 0x04, 20);
  std::memcpy(out.data() + hash + kHashHeader, user.thumbprint.data(), 20);
  std::memcpy(out.data() + base + fekOffset, user.wrapped_fek.data(),
              user.wrapped_fek.size());
}

// Appends a field (count, then entries) and returns its offset.
size_t AppendField(std::vector<BYTE>& out, std::span<const TestEfsEntry> users)
{
  const size_t offset = out.size();
  out.resize(offset + sizeof(DWORD), 0);
  Put32(out, offset, static_cast<DWORD>(users.size()));
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
      length = 16;
      break;
    case Algorithm::kAes192:
    case Algorithm::k3Des:
      length = 24;
      break;
    case Algorithm::kDesx:
      length = 16;
      break;
    case Algorithm::kAes256:
      length = 32;
      break;
  }

  std::vector<BYTE> key(length);
  for (size_t i = 0; i < length; ++i)
  {
    key[i] = static_cast<BYTE>((i * 7) + 0x21);
  }
  return key;
}

std::vector<BYTE> MakeFekBlob(Algorithm algorithm, std::span<const BYTE> key)
{
  std::vector<BYTE> blob(16 + key.size(), 0);
  Put32(blob, 0, static_cast<DWORD>(key.size()));
  Put32(blob, 4, static_cast<DWORD>(key.size()));
  Put32(blob, 8, static_cast<DWORD>(algorithm));
  std::copy(key.begin(), key.end(), blob.begin() + 16);
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
  ULONGLONG state = 0x9E3779B97F4A7C15ULL;
  for (BYTE& byte : bytes)
  {
    state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
    byte = static_cast<BYTE>(state >> 56);
  }
  return bytes;
}

std::array<BYTE, 20> TestThumbprint(BYTE seed)
{
  std::array<BYTE, 20> thumbprint{};
  for (size_t i = 0; i < thumbprint.size(); ++i)
  {
    thumbprint[i] = static_cast<BYTE>(seed + (i * 13));
  }
  return thumbprint;
}

std::vector<BYTE> MakeEfsStream(std::span<const TestEfsEntry> users,
                                std::span<const TestEfsEntry> recovery)
{
  // The header ends where the first field starts, as in a real stream.
  constexpr size_t kHeaderSize = 0x54;
  std::vector<BYTE> out(kHeaderSize, 0);
  Put32(out, 0x08, 2);

  if (!users.empty())
  {
    Put32(out, 0x40, static_cast<DWORD>(AppendField(out, users)));
  }
  if (!recovery.empty())
  {
    Put32(out, 0x44, static_cast<DWORD>(AppendField(out, recovery)));
  }
  Put32(out, 0x00, static_cast<DWORD>(out.size()));
  return out;
}

void TestKeyProvider::Add(const std::array<BYTE, 20>& thumbprint,
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
    if (std::equal(thumbprint.begin(), thumbprint.end(),
                   known.thumbprint.begin(), known.thumbprint.end()) &&
        std::equal(wrappedFek.begin(), wrappedFek.end(),
                   known.wrapped_fek.begin(), known.wrapped_fek.end()))
    {
      return known.blob;
    }
  }
  return std::nullopt;
}

}  // namespace NtfsBrowserTests
