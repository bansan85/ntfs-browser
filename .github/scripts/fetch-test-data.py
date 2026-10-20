#!/usr/bin/env python3
"""Fetches the forensic image corpora the integration tests read.

Three steps, runnable separately so CI can split them across machines:

  download    DFTT zips, the NPS ntfs1-gen2 E01 (converted to raw with
              ewfexport, from libewf), and a clone of msuhanov/ntfs-samples.
  decompress  Every ntfs-samples *.raw.gz, 4.3 GiB in all.
  external    A selection of NTFS files from three forensic projects (see
              specs/006-external-corpora-recovery/research.md): dfir_ntfs,
              Eric Zimmerman's MFT (none today) and DFIRArtifactMuseum. Each
              project is cloned shallowly, without blobs, at a pinned
              revision, with a sparse checkout of the kept paths only: the
              museum's other 1.4 GiB are never downloaded. Every kept file is
              checked against its SHA-256. A .gz is decompressed with the
              Python standard library, a .7z with 7zz, 7z, 7za or 7zr from the
              PATH. Without one of them, only the .7z files are skipped (an
              error with --require-7z). The clone is deleted afterwards.

ntfs.tgz (ntfs.raw, 64 GiB) and ntfs_extremely_fragmented_mft.rar (256 GiB,
not even in the repo) are too large for a CI runner: they stay compressed, and
their tests skip.

Every step skips an output that already exists. The resulting layout is the
one NTFS_BROWSER_TEST_DATA_DIR expects:

  <dest>/dftt/10-ntfs-autodetect/10-ntfs-part{1,2,3}.dd
  <dest>/dftt/3-kwsrch-ntfs/ntfs-img-kw-1.dd
  <dest>/dftt/7-undel-ntfs/7-ntfs-undel.dd
  <dest>/nps-2009-ntfs1/ntfs1-gen2.raw
  <dest>/ntfs-samples/*.raw
  <dest>/dfir-ntfs/test_data/...
  <dest>/zimmerman-mft/...
  <dest>/dfir-artifact-museum/Windows/NTFSArtifacts/...

The external layout keeps each file's path in its project, minus a .gz or .7z
suffix. NTFS_BROWSER_TEST_DFIR_NTFS_DIR, NTFS_BROWSER_TEST_ZIMMERMAN_MFT_DIR and
NTFS_BROWSER_TEST_DFIR_MUSEUM_DIR name the three folders.
"""

import argparse
import gzip
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path
from typing import NamedTuple, Optional

DFTT_ZIPS = ["10b-ntfs-autodetect", "3-kwsrch-ntfs", "7-undel-ntfs"]
DFTT_URL = "https://prdownloads.sourceforge.net/dftt/{}.zip?download"

NPS_E01_URL = (
    "https://downloads.digitalcorpora.org/corpora/drives/nps-2009-ntfs1/"
    "ntfs1-gen2.E01"
)
# MD5 of the raw image: the one ewfexport records in its own .info file.
NPS_RAW_MD5 = "27a9053e35aac74608493e755353cb95"

NTFS_SAMPLES_REPO = "https://github.com/msuhanov/ntfs-samples"

# Bytes copied at a time: large enough to keep Python's per-call cost negligible.
READ_CHUNK = 4 * 1024 * 1024


class Item(NamedTuple):
    """One kept file of an external project."""

    # Path in the project, with forward slashes.
    path: str
    # SHA-256 of that file as the project stores it.
    sha256: str
    # "gz" or "7z" when the file is an archive of one file, else None.
    unpack: Optional[str] = None
    # SHA-256 of the extracted file, when unpack is set.
    content_sha256: Optional[str] = None

    def output(self):
        """Path of the file this item leaves in the corpus."""
        if self.unpack is None:
            return self.path
        return self.path[: -len("." + self.unpack)]


class Project(NamedTuple):
    """An external project, cloned only for its kept files."""

    # Folder under <dest>: the one its CMake variable defaults to.
    folder: str
    url: str
    # Full commit hash: a pinned revision, so the hashes below stay valid.
    revision: str
    items: tuple


# The kept files and their hashes come from the selection report
# (specs/006-external-corpora-recovery/research.md, section 4). The four .fls
# listings are the exception: the report hashed a checkout with CRLF line ends
# (autocrlf), while the repository stores LF. Their hashes here are the stored
# bytes, which the clone below keeps as they are.
EXTERNAL_PROJECTS = (
    Project(
        "dfir-ntfs",
        "https://github.com/msuhanov/dfir_ntfs",
        "ec3ae0884048ade106c91a68dab7c9fb3af393bf",
        (
            Item(
                "test_data/nist-hacking-case.mft",
                "01ee1ecdf7206b82354cfa51090272df057d04a0114005388791007a98cc2e87",
            ),
            Item(
                "test_data/nist-hacking-case.fls",
                "bd8f1f87449bf66b7e8e0200614e2b56ee0e50ff403de0de9b9ef2bfce07b79b",
            ),
            Item(
                "test_data/4k-large.mft",
                "7b139535fc998309da6cc84962b066e1c7dd738c324a9652c18db3654105cc6d",
            ),
            Item(
                "test_data/4k-large.fls",
                "f4f5f8c49109f36000438dd17c0b1b16fdd544817b907702466585982064d9d3",
            ),
            Item(
                "test_data/orphan.mft",
                "7628aefb1f74bab31978d970a8ed5a48fb1cd39a82835923e7a6c8baa804f32d",
            ),
            Item(
                "test_data/orphan.fls",
                "581d1e8964454ff3392f9996b067d7a575ced84bb06b0a198ac5cd3201cb022f",
            ),
            Item(
                "test_data/deleted.mft",
                "9a9132fd2be0ef23771e4290f35db92244093bc3db0138ffab302ff1375d1959",
            ),
            Item(
                "test_data/unicode.mft",
                "83398d1241d8d582d44a87c37244ebdb69b83452c48bb79551fad263e5cfc165",
            ),
            Item(
                "test_data/unicode.fls",
                "8803725dc4e243aaaa1027a04fe17e4e4d991dd6b3732de3d48a486afd037d20",
            ),
            Item(
                "test_data/mapping_pairs_1.mft",
                "71e5fad1c7c3a5dffd6c5b2b8536ffca7116dd5295227b328ef8ba0b52e57cd2",
            ),
            Item(
                "test_data/slack.mft",
                "f8320740cf5951e239376d3a5a756663b828962d6f0c369ee9216a1b3f1ce046",
            ),
            Item(
                "test_data/slack-2.mft",
                "3b94f31c23eb6ab720319a01cc248bccda2635869b3b5d69c4872a814f01efbb",
            ),
            Item(
                "test_data/slack-3.mft",
                "d1c5339cf86c5d1e19b5c5c3d1b31acddde182e7dfa00bd2b9e465b876d5a10d",
            ),
            Item(
                "test_data/ntfs_frag_mft.bin",
                "d6ad40d2b07004973f8911c77220c08123d64bcd365f03434bc27ffd8fdf45fb",
            ),
            Item(
                "test_data/512.boot",
                "5ed186282ebf7496e8c17088840de6957fdcf9b09aaabd155f0730281c87d711",
            ),
            Item(
                "test_data/4k.boot",
                "e25de7b1b8c0d8cb1e3edab9a383c1544d237768a8222171a07002ab83d8a8d6",
            ),
            Item(
                "test_data/64k.boot",
                "3a42a02a8d8362e3229e2cacd29d926f2dcb64a3a536beccec743f0ee3b075d6",
            ),
            Item(
                "test_data/128k.boot",
                "422d7fd8f3320cc6d2b21baef78b950cdc7ae8ef0a890b8fe24d0393149e73a7",
            ),
            Item(
                "test_data/4kn.boot",
                "25eab9a6f6ce8347c5a1f65202d0f0785a5eab26cd1b1eab959693750b20de84",
            ),
            Item(
                "test_data/nobootcode.boot",
                "a8a60f7009d877fc4ddebb69d12ea9d3e7d477e757a9b268596d38a893ebc540",
            ),
            Item(
                "test_data/4k-large.mftmirr",
                "79975223762176f505bbf80d4559a50a6b775cef8e50bfb816b5fafc5ba3960c",
            ),
            Item(
                "test_data/boot.mftmirr",
                "c514f6b808915e4a69adc0cd71edcd3eb3e99434af3b853a2f0d1cdf92200878",
            ),
            Item(
                "test_data/usnjrnlj.bin",
                "5026fd52dd18c80fe48284876c34ad9dad8c4b894c6caed6aecca92e7c3f9453",
            ),
            Item(
                "test_data/usnjrnlj.fsutil.txt",
                "4ad0e27fb8d8bb33f687aa2e1ee828f1faf23e68803b6887afec699d8bda3d2b",
            ),
            Item(
                "test_data/ntfs_index.raw.gz",
                "22bacc8492e984290470380af43f989fdea8bf733f9bc6c59b4bc85d5792c967",
                "gz",
                "4b05a6adc5c091da4faa5de53adaeacc03c7bfeac86291aef5c271bce6be91a2",
            ),
            Item(
                "test_data/ntfs3_attrlist.raw.gz",
                "7bb3249fa76b102274821f832f87f036b1f08f1961d07e1ad4dd0ea967770533",
                "gz",
                "66ebe43fa04e401abbbac2e47eb922ad321cbb211cb8fd9e687d4c93b76d730a",
            ),
            Item(
                "test_data/LogFile_7.bin",
                "9b8948dc5b8b66e93f480a79eacb4440e8c6e939511ec35957222962379390d7",
            ),
            Item(
                "test_data/LogFile_10.bin",
                "a3e908923404ae806f755fb223a62b2838ca59a38eca49a32c1cb17ada6220c5",
            ),
            Item(
                "test_data/LogFile_10_downgraded.bin",
                "162fb28e4956ff6359e76325f8f0e61af1c720dabac9770992bc507abe2c7e41",
            ),
            Item(
                "test_data/LogFile_10_large.bin",
                "f7921e4f872f02a6d9d2e81e72d45251bf2045d03dd3586d3804d1570ea447aa",
            ),
            Item(
                "test_data/LogFile_empty.bin",
                "2d864c0b789a43214eee8524d3182075125e5ca2cd527f3582ec87ffd94076bc",
            ),
            Item(
                "test_data/rcrd_dax.bin",
                "1f63f37f89f3c59b3baf5b3f1ccf0801f4396381ab03ddafd8c66159397a3d30",
            ),
        ),
    ),
    # Nothing is taken from it: tdungan comes from the museum's 7z, so that
    # each file is fetched once, from one project.
    Project(
        "zimmerman-mft",
        "https://github.com/EricZimmerman/MFT",
        "4ba3fb14a20d90d89ee267a2659d9967fd613392",
        (),
    ),
    Project(
        "dfir-artifact-museum",
        "https://github.com/AndrewRathbun/DFIRArtifactMuseum",
        "fdcb1fab0c7b00e89129668d9c30174dd4ea3e5b",
        (
            Item(
                "Windows/NTFSArtifacts/MFT/Win2012R2/StolenSzechuan/$MFT",
                "a961e2f617f4c649429ec917fcddfadc54f7d74dc46b8510666313a3fb17ab1d",
            ),
            Item(
                "Windows/NTFSArtifacts/MFT/Win10/RathbunVM/$MFT.7z",
                "a6355e8075367445390c6ec03b8958ff7db7b0ca5212e7b255727d1255320472",
                "7z",
                "3abde16a546e991b83602b165e0a862b2dda4de5deb1001b612b71c277d482f0",
            ),
            Item(
                "Windows/NTFSArtifacts/MFT/Win11/RathbunVM/$MFT.7z",
                "bc817b8f244eb87a96b23d188fdd2cf37730405a2532ee011a0def7be9a11987",
                "7z",
                "3e1324d0289e6725f55ab63aaada3f2ab00f6ba1673ffba4fb4d836ac231c699",
            ),
            Item(
                "Windows/NTFSArtifacts/MFT/Unknown/xw/$MFT",
                "2811d925d87be7e90cdc7943432a32b8c22f141c4eeb13ca708e0a67f12cfe08",
            ),
            Item(
                "Windows/NTFSArtifacts/MFT/Unknown/NIST/DFR-16/$MFT",
                "9d42474a778c6c5c817c8b0ec59607df637eff24917e6266602d78fbd97b95f6",
            ),
            Item(
                "Windows/NTFSArtifacts/Boot/Win2012R2/StolenSzechuan/$Boot",
                "91958aa767d3b79ecd89e52dd17411834088bc880f868d5e576988cfe049041b",
            ),
            Item(
                "Windows/NTFSArtifacts/J/Win2012R2/StolenSzechuan/$J",
                "3927fdb4559997abec7d46f83ef9481607808ae79a5dae574799e2dbb5332d15",
            ),
            Item(
                "Windows/NTFSArtifacts/J/Win10/BelkasoftCTF_InisderThreat/$J",
                "847392a1571120d9388d59302bc397cacc1d09c02fd64505c8abc68a00d16278",
            ),
            Item(
                "Windows/NTFSArtifacts/J/Win11/RathbunVM/$J",
                "62c06bfc506e45918d7c32993bbcde9cfe5b1b08cc20760f50aad693659b8ead",
            ),
            Item(
                "Windows/NTFSArtifacts/J/Server2022/APTSimulatorVM/$J",
                "1e0faaeebcaef3a7bee9e7abb2ef570a8a1921cb76fa496a6aefbeb4ec62ac1c",
            ),
            Item(
                "Windows/NTFSArtifacts/$I30/EricZimmerman/Start/$I30",
                "070caf5b4f88a7940537506db1a470a3bbf3b1bf94562b699b20cc74eee223d7",
            ),
            Item(
                "Windows/NTFSArtifacts/$I30/EricZimmerman/FirstDelete/$I30",
                "a14cd6b404bc4a403529892d99383934076917c6fa5dbb955d64922993c37f35",
            ),
            Item(
                "Windows/NTFSArtifacts/$I30/EricZimmerman/SecondDelete/$I30",
                "6474f4c31fbcdca5c5cb93669047eaea896cf2186d7cacd6dd9774c627d19652",
            ),
            Item(
                "Windows/NTFSArtifacts/LogFile/Win2012R2/StolenSzechuan/$LogFile",
                "3c7d35d9d974c9a6215c45428d2193fe2530f5007a1b8523bac9edd5581c4ac7",
            ),
            Item(
                "Windows/NTFSArtifacts/LogFile/Win10/BelkasoftCTF_InsiderThreat/$LogFile",
                "dc86b27e630d00c9ae4b779fa4a0651e4c0dcae303273e92b273721190c32dcb",
            ),
            Item(
                "Windows/NTFSArtifacts/LogFile/Win11/RathbunVM/$LogFile",
                "bc0f1510964b46b57339abd34722c1809e59074512d709d195cd0941c20bd4f4",
            ),
            Item(
                "Windows/NTFSArtifacts/MFT/Unknown/tdungan/$MFT.7z",
                "a57683ab5913a8b5af985b5dd1f70ebc35d4f7ca88f8d276811704bbe6104b79",
                "7z",
                "bd96a16ee812f9c7724ef31a87686a71eada6c80a4e5131a94945e42500a6a2e",
            ),
        ),
    ),
)

# Extractors for a .7z, in the order they are looked up on the PATH.
SEVEN_ZIP_TOOLS = ("7zz", "7z", "7za", "7zr")



def log(message):
    print(message, flush=True)


def download(url, target):
    log(f"Downloading {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "curl/8"})
    with urllib.request.urlopen(request) as response, open(target, "wb") as out:
        shutil.copyfileobj(response, out, READ_CHUNK)


def fetch_dftt(dest):
    dftt = dest / "dftt"
    dftt.mkdir(parents=True, exist_ok=True)
    for name in DFTT_ZIPS:
        # 10b-ntfs-autodetect.zip unpacks into 10-ntfs-autodetect/.
        unpacked = dftt / name.replace("10b-", "10-")
        if unpacked.exists():
            continue
        archive = dftt / f"{name}.zip"
        download(DFTT_URL.format(name), archive)
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(dftt)
        archive.unlink()


def md5_of(path):
    digest = hashlib.md5()
    with open(path, "rb") as f:
        while chunk := f.read(READ_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def fetch_nps(dest):
    nps = dest / "nps-2009-ntfs1"
    raw = nps / "ntfs1-gen2.raw"
    if raw.exists():
        return
    ewfexport = shutil.which("ewfexport")
    if ewfexport is None:
        sys.exit(
            "ewfexport not found: install libewf (ewf-tools on Debian/Ubuntu),"
            " or run the download step under WSL"
        )
    nps.mkdir(parents=True, exist_ok=True)
    e01 = nps / "ntfs1-gen2.E01"
    download(NPS_E01_URL, e01)
    with tempfile.TemporaryDirectory(dir=nps) as tmp:
        target = Path(tmp) / "ntfs1-gen2"
        log(f"Converting {e01.name} to raw")
        subprocess.run(
            [ewfexport, "-u", "-q", "-f", "raw", "-t", str(target), str(e01)],
            check=True,
        )
        # Some libewf versions append ".raw" to the target, others do not.
        produced = next(p for p in (target.with_suffix(".raw"), target) if p.exists())
        if md5_of(produced) != NPS_RAW_MD5:
            sys.exit(f"{produced}: MD5 mismatch, expected {NPS_RAW_MD5}")
        produced.replace(raw)
    e01.unlink()


def fetch_ntfs_samples(dest):
    samples = dest / "ntfs-samples"
    if samples.exists():
        return
    log(f"Cloning {NTFS_SAMPLES_REPO}")
    subprocess.run(
        ["git", "clone", "--depth", "1", NTFS_SAMPLES_REPO, str(samples)],
        check=True,
    )


def decompress_ntfs_samples(dest):
    samples = dest / "ntfs-samples"
    if not samples.exists():
        sys.exit(f"{samples} missing: run the download step first")
    for archive in sorted(samples.glob("*.raw.gz")):
        raw = archive.with_suffix("")
        if raw.exists():
            continue
        log(f"Decompressing {archive.name}")
        partial = raw.with_name(raw.name + ".part")
        with gzip.open(archive, "rb") as source, open(partial, "wb") as out:
            shutil.copyfileobj(source, out, READ_CHUNK)
        partial.replace(raw)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(READ_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def check_sha256(path, expected, project, name):
    actual = sha256_of(path)
    if actual != expected:
        sys.exit(
            f"{project.folder}: {name}: SHA-256 mismatch, expected {expected},"
            f" got {actual}"
        )


def make_writable_and_retry(function, path, _error):
    # git marks its pack and object files read-only, which blocks rmtree on
    # Windows.
    os.chmod(path, stat.S_IWRITE)
    function(path)


def remove_tree(path):
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=make_writable_and_retry)
    else:
        shutil.rmtree(path, onerror=make_writable_and_retry)


def find_seven_zip():
    for name in SEVEN_ZIP_TOOLS:
        found = shutil.which(name)
        if found is not None:
            return found
    # A Windows install of 7-Zip does not add itself to the PATH.
    program_files = os.environ.get("ProgramFiles")
    if program_files:
        installed = Path(program_files) / "7-Zip" / "7z.exe"
        if installed.exists():
            return str(installed)
    return None


def git(clone, *args):
    # No end-of-line conversion: the hashes are those of the stored bytes.
    subprocess.run(
        [
            "git",
            "-c",
            "core.autocrlf=false",
            "-c",
            "core.longpaths=true",
            "-c",
            "advice.detachedHead=false",
            "-C",
            str(clone),
            *args,
        ],
        check=True,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
    )


def sparse_clone(project, items, clone):
    """Checks out only the paths of items, at the project's pinned revision."""
    log(f"Cloning {project.url} at {project.revision[:12]}: {len(items)} file(s)")
    clone.mkdir(parents=True)
    git(clone, "init", "-q")
    git(clone, "remote", "add", "origin", project.url)
    # A promisor remote lets the fetch leave the blobs out.
    git(clone, "config", "remote.origin.promisor", "true")
    git(clone, "config", "remote.origin.partialclonefilter", "blob:none")
    git(
        clone,
        "fetch",
        "-q",
        "--depth",
        "1",
        "--filter=blob:none",
        "origin",
        project.revision,
    )
    git(clone, "sparse-checkout", "init", "--no-cone")
    # One anchored pattern per file; no path of the list holds a glob character.
    patterns = [f"/{item.path}" for item in items]
    git(clone, "sparse-checkout", "set", "--no-cone", *patterns)
    git(clone, "checkout", "-q", "FETCH_HEAD")


def place(source, target):
    """Moves source to target without ever leaving a partial file at target."""
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part")
    shutil.copyfile(source, partial)
    partial.replace(target)


def unpack_gz(archive, target, expected, project, name):
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part")
    with gzip.open(archive, "rb") as source, open(partial, "wb") as out:
        shutil.copyfileobj(source, out, READ_CHUNK)
    check_sha256_or_discard(partial, expected, project, name)
    partial.replace(target)


def unpack_7z(tool, archive, target, expected, project, name):
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent, prefix=".extract-") as tmp:
        subprocess.run(
            [tool, "x", "-y", "-bd", f"-o{tmp}", str(archive)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        extracted = [p for p in Path(tmp).rglob("*") if p.is_file()]
        if len(extracted) != 1:
            sys.exit(f"{project.folder}: {name}: expected one file in the archive")
        partial = target.with_name(target.name + ".part")
        shutil.move(extracted[0], partial)
    check_sha256_or_discard(partial, expected, project, name)
    partial.replace(target)


def check_sha256_or_discard(partial, expected, project, name):
    try:
        check_sha256(partial, expected, project, name)
    except SystemExit:
        partial.unlink()
        raise


def fetch_external_project(project, dest, seven_zip, skipped):
    root = dest / project.folder
    pending = [i for i in project.items if not (root / i.output()).exists()]
    if not seven_zip:
        # An archive that cannot be opened is no reason to clone the project.
        skipped.extend(
            f"{project.folder}/{i.path}" for i in pending if i.unpack == "7z"
        )
        pending = [i for i in pending if i.unpack != "7z"]
    if not pending:
        return
    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        dir=dest, prefix=".clone-", ignore_cleanup_errors=True
    ) as tmp:
        clone = Path(tmp) / "clone"
        try:
            sparse_clone(project, pending, clone)
            for item in pending:
                source = clone / item.path
                if not source.is_file():
                    sys.exit(
                        f"{project.folder}: {item.path}: absent from the clone"
                    )
                check_sha256(source, item.sha256, project, item.path)
                target = root / item.output()
                log(f"Extracting {project.folder}/{item.output()}")
                if item.unpack == "gz":
                    unpack_gz(
                        source, target, item.content_sha256, project, item.path
                    )
                elif item.unpack == "7z":
                    unpack_7z(
                        seven_zip,
                        source,
                        target,
                        item.content_sha256,
                        project,
                        item.path,
                    )
                else:
                    place(source, target)
        finally:
            if clone.exists():
                remove_tree(clone)


def fetch_external(dest, require_seven_zip):
    seven_zip = find_seven_zip()
    skipped = []
    for project in EXTERNAL_PROJECTS:
        fetch_external_project(project, dest, seven_zip, skipped)
    if skipped:
        message = (
            "7zz, 7z, 7za or 7zr not found on the PATH (install 7-Zip):"
            f" {len(skipped)} archive(s) skipped"
        )
        if require_seven_zip:
            sys.exit(message)
        log(f"Warning: {message}")
        for path in skipped:
            log(f"  {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "step",
        choices=["download", "decompress", "external", "all"],
        nargs="?",
        default="all",
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "test-data",
        help="corpus root (default: test-data/ at the repository root)",
    )
    parser.add_argument(
        "--require-7z",
        action="store_true",
        help="fail, instead of skipping the .7z files, when no 7-Zip is found",
    )
    args = parser.parse_args()
    dest = args.dest.resolve()
    if args.step in ("download", "all"):
        fetch_dftt(dest)
        fetch_ntfs_samples(dest)
        # Last: it is the only step that needs ewfexport.
        fetch_nps(dest)
    if args.step in ("decompress", "all"):
        decompress_ntfs_samples(dest)
    if args.step in ("external", "all"):
        fetch_external(dest, args.require_7z)


if __name__ == "__main__":
    main()
