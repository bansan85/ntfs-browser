#!/usr/bin/env python3
"""Fetches the forensic image corpora the integration tests read.

Two steps, runnable separately so CI can split them across machines:

  download    DFTT zips, the NPS ntfs1-gen2 E01 (converted to raw with
              ewfexport, from libewf), and a clone of msuhanov/ntfs-samples.
  decompress  Every ntfs-samples *.raw.gz, 4.3 GiB in all.

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
"""

import argparse
import gzip
import hashlib
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "step", choices=["download", "decompress", "all"], nargs="?", default="all"
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "test-data",
        help="corpus root (default: test-data/ at the repository root)",
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


if __name__ == "__main__":
    main()
