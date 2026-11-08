#!/usr/bin/env python3
"""Makes the resource archives of the corpus traces (dev time only).

NtfsFuzzGen never reads a forensic image. A trace is the bytes a corpus test
read from its image. The generator rebuilds a sparse volume image from three
layers and runs the same reads through the library:

  1. zeros, for every byte that is not named below;
  2. the layers, computed by code (a "layers" file names where they sit):
       ptrn ADDR LENGTH   the repeating "PTRN" fill of the PTRN images
       upcase ADDR UNITS  an identity $UpCase table of UNITS 16-bit words;
  3. the residue: the bytes where the real image differs from layers 1 and 2.

The addresses are volume relative, the way the library asks for them. A
"reads" file in corpus-reads/ lists the (address, length) of every read of one
trace. Make it again when the read order of the library changes: run the
corpus test with NTFS_BROWSER_RECORD_TRACES=1 and a temporary patch of
Test::DiskReaders::RecordingDiskReader::ReadInto that prints "addr size" per
read.

One archive is made per corpus. An archive keeps the members that this run
does not make again, so a run with --only changes only what it names.

  make-corpus-residue.py --images DIR [--only TRACE ...]
"""

import argparse
import io
import lzma
import struct
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Patches closer than this many bytes are merged: a record header costs more.
MERGE_GAP = 16

PTRN = b"PTRN"

# archive, trace, image path under --images, partition offset, layers.
TRACES = [
    ("dftt", "dftt-autodetect-part1", "10-ntfs-autodetect/10-ntfs-part1.dd", 0,
     [("upcase", 24720896, 65536)]),
    # The UFS2 file system formatted over this partition replaced the second
    # half of the table.
    ("dftt", "dftt-autodetect-part2", "10-ntfs-autodetect/10-ntfs-part2.dd", 0,
     [("upcase", 24736768, 26112)]),
    ("dftt", "dftt-autodetect-part3", "10-ntfs-autodetect/10-ntfs-part3.dd", 0,
     [("upcase", 41195520, 65536)]),
    ("dftt", "dftt-kwsrch", "3-kwsrch-ntfs/ntfs-img-kw-1.dd", 0, []),
    ("dftt", "dftt-kwsrch-deleted", "3-kwsrch-ntfs/ntfs-img-kw-1.dd", 0, []),
    ("dftt", "dftt-undel", "7-undel-ntfs/7-ntfs-undel.dd", 0, []),
    ("ntfs-samples", "ntfs-fragmented-mft",
     "ntfs_extremely_fragmented_mft.raw", 1048576, []),
    ("ntfs-samples", "ntfs-si-vs-fn", "ntfs-si-vs-fn.raw", 65536,
     [("upcase", 12288, 65536)]),
    ("ntfs-samples", "ntfs", "ntfs.raw", 1048576, [("upcase", 12288, 65536)]),
    ("ntfs-samples", "ntfs-lastaccess", "ntfs-lastaccess.raw", 65536,
     [("upcase", 12288, 65536), ("ptrn", 176128, 2060288),
      ("ptrn", 2273280, 270336)]),
    ("ntfs-samples", "ntfs-ptrn", "ntfs-ptrn.raw", 65536,
     [("upcase", 12288, 65536), ("ptrn", 176128, 2060288),
      ("ptrn", 2273280, 270336)]),
]


def read_intervals(trace):
    """The union of the byte ranges of the reads, as sorted [start, end)."""
    ranges = []
    for line in (HERE / "corpus-reads" / (trace + ".reads")).read_text().split("\n"):
        if line.strip():
            address, length = (int(x) for x in line.split())
            ranges.append((address, address + length))
    ranges.sort()
    merged = []
    for start, end in ranges:
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return merged


def predicted(layers, start, end):
    """The bytes the layers give for [start, end): zeros where none applies."""
    out = bytearray(end - start)
    for kind, address, size in layers:
        length = size if kind == "ptrn" else size * 2
        low = max(start, address)
        high = min(end, address + length)
        for position in range(low, high):
            if kind == "ptrn":
                out[position - start] = PTRN[position % 4]
            else:
                unit = (position - address) // 2
                out[position - start] = (unit >> (8 * (position & 1))) & 0xFF
    return out


def make_residue(image, offset, layers, intervals):
    """The patch records for one trace."""
    out = bytearray()
    for start, end in intervals:
        image.seek(offset + start)
        real = image.read(end - start)
        if len(real) != end - start:
            raise RuntimeError("the image is shorter than the reads")
        expected = predicted(layers, start, end)
        position = 0
        total = end - start
        while position < total:
            if real[position] == expected[position]:
                position += 1
                continue
            first = position
            last = position
            position += 1
            while position < total and position - last <= MERGE_GAP:
                if real[position] != expected[position]:
                    last = position
                position += 1
            out += struct.pack("<QI", start + first, last + 1 - first)
            out += real[first : last + 1]
            position = last + 1
    return bytes(out)


def make_layers(layers):
    return "".join("%s %d %d\n" % layer for layer in layers).encode("ascii")


def old_members(archive):
    members = {}
    if archive.exists():
        with tarfile.open(archive, "r:xz") as old:
            for member in old.getmembers():
                members[member.name] = old.extractfile(member).read()
    return members


def write_archive(archive, members):
    with tarfile.open(archive, "w:xz", format=tarfile.PAX_FORMAT,
                      preset=9 | lzma.PRESET_EXTREME) as out:
        for name in sorted(members):
            info = tarfile.TarInfo(name)
            info.size = len(members[name])
            info.mtime = 0
            info.mode = 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            out.addfile(info, io.BytesIO(members[name]))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--images", type=Path, required=True,
                        help="folder of the forensic images")
    parser.add_argument("--only", nargs="*", default=[],
                        help="traces to make again (default: all)")
    args = parser.parse_args()

    members = {}
    for stem in sorted({row[0] for row in TRACES}):
        members[stem] = old_members(HERE / (stem + ".tar.xz"))
    for stem, trace, image_path, offset, layers in TRACES:
        if args.only and trace not in args.only:
            continue
        with open(args.images / image_path, "rb") as image:
            residue = make_residue(image, offset, layers, read_intervals(trace))
        members[stem][trace + ".layers"] = make_layers(layers)
        members[stem][trace + ".residue"] = residue
        print("%s: residue %d bytes" % (trace, len(residue)))
    for stem, files in members.items():
        archive = HERE / (stem + ".tar.xz")
        write_archive(archive, files)
        print("%s: %d bytes" % (archive.name, archive.stat().st_size))


if __name__ == "__main__":
    sys.exit(main())
