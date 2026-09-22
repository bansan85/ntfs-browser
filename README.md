# ntfs-browser
C++20 Fast NTFS browser under Windows.



## Based on Code Project [An NTFS Parser Lib](https://www.codeproject.com/Articles/81456/An-NTFS-Parser-Lib)

Project licensed under BSD-3c.

In `documentation/old` folder, there is an archive of the main web page of the project.

This project improves the historical software by:

  - fixing minor bugs,
  - rewriting it with C++20 coding style,
  - caching `ReadFile` in `CAttrNonResident::ReadClusters`,
  - reading LZNT1-compressed files and directories
    (`FILE_ATTRIBUTE_COMPRESSED`), which the original skipped entirely:
    compression units are decompressed transparently, so `ReadData()` keeps
    returning plain bytes,
  - reading EFS-encrypted files and directories (`FILE_ATTRIBUTE_ENCRYPTED`),
    which the original skipped as well. `ReadData()` decrypts `$DATA` streams
    transparently, when a key is available. See below.

## EFS encryption

The File Encryption Key (FEK) of a file is stored in its `$EFS` stream, once per
allowed user, wrapped with that user's RSA public key. The library needs the
matching private key to unwrap it. A key comes from an `Efs::IEfsKeyProvider`,
installed with `NtfsVolume::SetEfsKeyProvider()`:

  - by default, the personal certificate store of the current user
    (`CurrentUser\My`), on Windows. It is opened on the first decryption.
  - `Efs::MakePfxKeyProvider()`, for a key in a PFX file. The key is never
    imported into the user's key storage.
  - your own implementation of `IEfsKeyProvider`.
  - `SetEfsKeyProvider(nullptr)`, which turns decryption off. Encrypted
    directories still list, and an encrypted stream reads as `nullopt`, with a
    warning naming the cause.

The supported ciphers are AES-256, AES-192, AES-128, 3DES and DESX. Only AES-256
was checked against a real volume. The others are covered by known-answer and
round-trip tests. The DESX key layout is a guess, and untested on real data.
`Efs::SetCipherBackend()` picks the library that runs the cipher: Crypto++
(the default) or, on Windows, BCrypt. BCrypt has no DESX, so DESX always uses
Crypto++.

Limits: decryption is read-only, and there is no EFS on compressed data. Only a
`$DATA` stream flagged encrypted is decrypted. `$INDEX_ALLOCATION`, `$BITMAP`
and `$ATTRIBUTE_LIST` never are.

Crypto++ is a git submodule, linked statically and privately, even into a
shared `NtfsBrowser`. To use an installed copy instead, for example one from
vcpkg, configure with `-DNTFS_BROWSER_USE_INSTALLED_CRYPTOPP=ON`.
