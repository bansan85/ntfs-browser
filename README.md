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
`NtfsVolume::SetEfsCipherBackend()` picks the library that runs the cipher: Crypto++
(the default) or, on Windows, BCrypt. BCrypt has no DESX, so DESX always uses
Crypto++.

Limits: decryption is read-only, and there is no EFS on compressed data. Only a
`$DATA` stream flagged encrypted is decrypted. `$INDEX_ALLOCATION`, `$BITMAP`
and `$ATTRIBUTE_LIST` never are.

Crypto++ is a git submodule, linked statically and privately, even into a
shared `NtfsBrowser`. To use an installed copy instead, for example one from
vcpkg, configure with `-DNTFS_BROWSER_USE_INSTALLED_CRYPTOPP=ON`.

## Tests

The unit tests are Windows-only. Most of them build synthetic NTFS images in
memory and need nothing else:

```
cmake --preset static
cmake --build --preset static --parallel
ctest --test-dir build/static
```

### Forensic image corpora

The integration tests (Catch2 tag `[integration]`) read real NTFS images, too
large for this repository:

| Corpus | Source | Tests |
| --- | --- | --- |
| DFTT #3, #7, #10 | [dftt.sourceforge.net](https://dftt.sourceforge.net/): `3-kwsrch-ntfs.zip`, `7-undel-ntfs.zip`, `10b-ntfs-autodetect.zip` | `dftt-ntfs-*-tests.cpp` |
| nps-2009-ntfs1, generation 2 | [Digital Corpora](https://downloads.digitalcorpora.org/corpora/drives/nps-2009-ntfs1/): `ntfs1-gen2.E01`, converted to raw | `nps-ntfs1-*tests.cpp` |
| ntfs-samples | [msuhanov/ntfs-samples](https://github.com/msuhanov/ntfs-samples): each `*.raw.gz`, decompressed | `ntfs-samples-tests.cpp` |

A test whose image is missing is skipped. To fetch them all:

```
python .github/scripts/fetch-test-data.py
```

The script needs Python 3, `git`, and `ewfexport` from
[libewf](https://github.com/libyal/libewf) to convert the E01 image
(`sudo apt install ewf-tools` on Debian/Ubuntu). On Windows, run it under WSL,
or run `download` under WSL and `decompress` on Windows. It fills `test-data/`
at the repository root (gitignored), about 5.1 GiB:

```
test-data/
  dftt/10-ntfs-autodetect/10-ntfs-part{1,2,3}.dd
  dftt/3-kwsrch-ntfs/ntfs-img-kw-1.dd
  dftt/7-undel-ntfs/7-ntfs-undel.dd
  nps-2009-ntfs1/ntfs1-gen2.raw
  ntfs-samples/ntfs-{2m,lastaccess,ptrn,ramslack,si-vs-fn}.raw
```

Two ntfs-samples images are left out, and their tests skip:

  - `ntfs.raw` needs 64 GiB once `ntfs.tgz` is extracted. Extract it into the
    ntfs-samples directory by hand to run its test.
  - `ntfs_extremely_fragmented_mft.raw` needs 256 GiB. Its RAR archive is not
    in the repository: its `ReadMe.md` links to it. Point
    `NTFS_BROWSER_TEST_FRAGMENTED_MFT_IMAGE` at the extracted image.

These CMake cache variables locate the images:

| Variable | Default |
| --- | --- |
| `NTFS_BROWSER_TEST_DATA_DIR` | `<repository>/test-data` |
| `NTFS_BROWSER_TEST_DFTT_DIR` | `<NTFS_BROWSER_TEST_DATA_DIR>/dftt` |
| `NTFS_BROWSER_TEST_NPS_NTFS1_DIR` | `<NTFS_BROWSER_TEST_DATA_DIR>/nps-2009-ntfs1` |
| `NTFS_BROWSER_TEST_NTFS_SAMPLES_DIR` | `<NTFS_BROWSER_TEST_DATA_DIR>/ntfs-samples` |
| `NTFS_BROWSER_TEST_FRAGMENTED_MFT_IMAGE` | `<ntfs-samples dir>/ntfs_extremely_fragmented_mft.raw` |

For example, with the images already on another drive:

```
cmake --preset static -DNTFS_BROWSER_TEST_DATA_DIR=D:/ntfs-images
```

The four per-corpus variables can also come from the environment, under the
same name. A cache variable set with `-D` wins over the environment. The
environment reaches every preset without editing `CMakePresets.json`. In VS
Code, set it in `cmake.configureEnvironment`, in your own
`.vscode/settings.json` (gitignored):

```json
"cmake.configureEnvironment": {
  "NTFS_BROWSER_TEST_DFTT_DIR": "D:/ntfs-images/dftt",
  "NTFS_BROWSER_TEST_NTFS_SAMPLES_DIR": "E:/ntfs-samples"
}
```

CMake reads the environment only when it configures. After changing a value,
configure again.

`-DNTFS_BROWSER_REQUIRE_TEST_DATA=ON` fails, instead of skipping, a test whose
image the script provides. CI sets it.

### CI

The `test-data` job runs the script's `download` step on Ubuntu, and caches
`test-data/`. The cache key is the script's hash, so editing the script fetches
everything again. Each Windows leg restores the cache, runs `decompress`, and
runs `ctest` with `NTFS_BROWSER_REQUIRE_TEST_DATA=ON`.
