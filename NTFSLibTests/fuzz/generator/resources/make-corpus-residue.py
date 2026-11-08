#!/usr/bin/env python3
"""Makes the resource archives of the corpus traces (dev time only).

NtfsFuzzGen never reads a forensic image at run time. A trace is the bytes a
corpus test read from its image. The generator rebuilds a sparse volume image
from layers and runs the same reads through the library:

  1. zeros, for every byte that is not named below;
  2. the layers, computed by code. A "layers" file names them, one per line
     (see LayerScript in the generator for the grammar):
       ptrn ADDR LENGTH   the repeating "PTRN" fill of the PTRN images
       upcase ADDR UNITS  an identity $UpCase table of UNITS 16-bit words
       stamp ADDR ...     a block repeated on every page
       boot, record, indx the boot sector, the file records and the index
                          blocks, from named fields (the structures of the
                          library lay them out, this script reads the values)
       text ADDR LENGTH file=N
                          the numbered lines of a log file, from its byte N
       blob ADDR name=NAME offset=N length=N
                          bytes of a file that the archive keeps once, in
                          every trace that reads it
       lznt1 ADDR file=N count=N [name=NAME]
                          chunks of a log file or of a blob, compressed with
                          LZNT1 the way Windows XP did
  3. the residue: the bytes where the real image differs from layers 1 and 2.

The addresses are volume relative, the way the library asks for them. A
"reads" file in corpus-reads/ lists the (address, length) of every read of one
trace. Make it again when the read order of the library changes: run the
corpus test with the image present (and NTFS_BROWSER_FORCE_TRACES unset) and a
temporary patch of Test::DiskReaders::PartitionDiskReader::ReadInto that
prints "addr size" per read. A test that opens the image several times keeps
the reads of the last open only.

The residue is the difference between the image and what NtfsFuzzGen predicts
(its --predict mode), so the layers have one implementation, in C++. Build
NtfsFuzzGen first and give its path with --generator.

One archive is made per corpus. An archive keeps the members that this run
does not make again, so a run with --only changes only what it names.

  make-corpus-residue.py --images DIR --generator NtfsFuzzGen [--only TRACE ...]
"""

import argparse
import io
import lzma
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Patches closer than this many bytes are merged: a record header costs more.
MERGE_GAP = 16

# Equal blocks of this many bytes are skipped at once when two buffers are
# compared. Only a speed-up.
SKIP_BLOCK = 4096

# A file record, and the sector that the update sequence array protects.
RECORD_BYTES = 1024
SECTOR_BYTES = 512

# An index block of the volumes that Windows formats with 4 KiB clusters.
INDEX_BLOCK_BYTES = 4096

# The boot area: the BPB takes the first 0x50 bytes of it, the boot code
# (shared by the volumes of one Windows version) the rest.
BOOT_AREA_BYTES = 4096
BOOT_FIELDS_BYTES = 0x50
BOOT_CODE_NAME = "windows-boot-code"

# What a Windows 10 boot sector holds in the bytes that NTFS does not use.
DEFAULT_UNUSED_MARKER = 0x00800080

# The files of an image whose bytes code cannot make are kept once in the
# archive, and the layers name them: a file is a "blob". A blob is a run of
# whole sectors of the image, found in the read set of the trace that reads the
# file in one piece. Files with a pattern are made by code: the text of a log
# file is "This is line N." for every N.
# archive: ([(blob name, address, bytes)], size of the log file)
FILE_DATA = {
    "nps": ([("20076517123273.pdf", 244974080, 1001984),
             ("NISTSP800-88_rev1.pdf", 238139904, 554496),
             ("NIST_logo.jpg", 258289152, 2560),
             ("report02-3.pdf", 241542144, 1422336)], 21888890),
}

# Bytes of text in an LZNT1 chunk.
CHUNK_BYTES = 4096

# A stamp is a patch that repeats at least this many times, at the same
# distance.
STAMP_MIN_COUNT = 3

# Which layer kinds a trace uses. "structure" turns on the boot sector, file
# records, index blocks and stamps: the script finds them in the bytes that the
# trace reads.
# archive, trace, image path under --images, partition offset, layers,
# structure.
TRACES = [
    ("dftt", "dftt-autodetect-part1", "10-ntfs-autodetect/10-ntfs-part1.dd", 0,
     [("upcase", 24720896, 65536)], False),
    # The UFS2 file system formatted over this partition replaced the second
    # half of the table.
    ("dftt", "dftt-autodetect-part2", "10-ntfs-autodetect/10-ntfs-part2.dd", 0,
     [("upcase", 24736768, 26112)], False),
    ("dftt", "dftt-autodetect-part3", "10-ntfs-autodetect/10-ntfs-part3.dd", 0,
     [("upcase", 41195520, 65536)], False),
    ("dftt", "dftt-kwsrch", "3-kwsrch-ntfs/ntfs-img-kw-1.dd", 0, [], False),
    ("dftt", "dftt-kwsrch-deleted", "3-kwsrch-ntfs/ntfs-img-kw-1.dd", 0, [],
     False),
    ("dftt", "dftt-undel", "7-undel-ntfs/7-ntfs-undel.dd", 0, [], False),
    ("ntfs-samples", "ntfs-fragmented-mft",
     "ntfs_extremely_fragmented_mft.raw", 1048576, [], True),
    ("ntfs-samples", "ntfs-si-vs-fn", "ntfs-si-vs-fn.raw", 65536,
     [("upcase", 12288, 65536)], True),
    ("ntfs-samples", "ntfs", "ntfs.raw", 1048576, [("upcase", 12288, 65536)],
     True),
    ("ntfs-samples", "ntfs-lastaccess", "ntfs-lastaccess.raw", 65536,
     [("upcase", 12288, 65536), ("ptrn", 176128, 2060288),
      ("ptrn", 2273280, 270336)], True),
    ("ntfs-samples", "ntfs-ptrn", "ntfs-ptrn.raw", 65536,
     [("upcase", 12288, 65536), ("ptrn", 176128, 2060288),
      ("ptrn", 2273280, 270336)], True),
    ("ntfs-samples", "ntfs-ramslack", "ntfs-ramslack.raw", 65536,
     [("upcase", 12288, 65536), ("ptrn", 204800, 8351744),
      ("ptrn", 351834112, 1011712)], True),
    ("ntfs-samples", "ntfs-2m", "ntfs-2m.raw", 65536, [], True),
    ("nps", "nps-ntfs1-raw", "ntfs1-gen2.raw", 0,
     [("upcase", 258452480, 65536)], True),
    ("nps", "nps-ntfs1-compressed", "ntfs1-gen2.raw", 0,
     [("upcase", 258452480, 65536)], True),
    ("nps", "nps-ntfs1-scan-threads", "ntfs1-gen2.raw", 0,
     [("upcase", 258452480, 65536)], True),
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


class Volume:
    """The bytes of the intervals that a trace reads, from the real image."""

    def __init__(self, image, offset, intervals):
        self.intervals = intervals
        self.data = {}
        for start, end in intervals:
            image.seek(offset + start)
            real = image.read(end - start)
            if len(real) != end - start:
                raise RuntimeError("the image is shorter than the reads")
            self.data[start] = real

    def get(self, address, length):
        """The bytes at address, or None when a read does not cover them."""
        for start, end in self.intervals:
            if start <= address and address + length <= end:
                return self.data[start][address - start : address - start + length]
        return None


def u16(raw, at):
    return struct.unpack_from("<H", raw, at)[0]


def u32(raw, at):
    return struct.unpack_from("<I", raw, at)[0]


def u64(raw, at):
    return struct.unpack_from("<Q", raw, at)[0]


def number(value):
    return "0x%x" % value


def name_fields(key, units):
    """name=TEXT for a plain name, name16=HEX otherwise."""
    text = units.decode("utf-16-le", errors="surrogatepass")
    plain = all(33 <= ord(c) <= 126 and c not in "=|" for c in text)
    if text and plain:
        return ["%s=%s" % (key, text)]
    if not text:
        return []
    return ["%s16=%s" % (key, units.hex())]


def fn_fields(value):
    """The $FILE_NAME fields of value as words, or None when it is not one."""
    if len(value) < 66 or len(value) != 66 + 2 * value[64]:
        return None
    words = ["parent=" + number(u64(value, 0)), "create=" + number(u64(value, 8)),
             "alter=" + number(u64(value, 16)), "mft=" + number(u64(value, 24)),
             "read=" + number(u64(value, 32)), "alloc=" + number(u64(value, 40)),
             "real=" + number(u64(value, 48)), "attrs=" + number(u32(value, 56)),
             "er=" + number(u32(value, 60)), "space=%d" % value[65]]
    return words + name_fields("name", value[66:])


def si_fields(value):
    """The $STANDARD_INFORMATION fields of value as words, or None."""
    if len(value) not in (48, 72):
        return None
    padded = value + bytes(72 - len(value))
    return ["len=%d" % len(value), "create=" + number(u64(padded, 0)),
            "alter=" + number(u64(padded, 8)), "mft=" + number(u64(padded, 16)),
            "read=" + number(u64(padded, 24)), "perm=" + number(u32(padded, 32)),
            "max_ver=%d" % u32(padded, 36), "ver=%d" % u32(padded, 40),
            "class=%d" % u32(padded, 44), "owner=%d" % u32(padded, 48),
            "security=%d" % u32(padded, 52), "quota=" + number(u64(padded, 56)),
            "usn=" + number(u64(padded, 64))]


def entry_lines(raw, offset, end):
    """The "entry" lines of the index entries in raw[offset:end], or None."""
    lines = []
    while offset + 16 <= end:
        ref, size, key_size, flags = struct.unpack_from("<QHHB", raw, offset)
        if size < 16 or offset + size > end:
            return None
        words = ["entry", "ref=" + number(ref), "flags=%d" % flags]
        if not flags & 2:
            fields = fn_fields(raw[offset + 16 : offset + 16 + key_size])
            if fields is None:
                return None
            words += fields
        if flags & 1:
            words.append("vcn=%d" % u64(raw, offset + size - 8))
        lines.append(" ".join(words))
        offset += size
        if flags & 2:
            return lines
    return None


def index_header_words(raw, header, default_alloc):
    """The words of an index header at raw[header:], relative offsets."""
    entry_offset, total, alloc = struct.unpack_from("<III", raw, header)
    words = []
    if alloc != default_alloc:
        words.append("alloc=%d" % alloc)
    return entry_offset, total, words


def attribute_lines(raw, offset):
    """The attribute lines of a file record (after fixups), or None."""
    lines = []
    while offset + 8 <= RECORD_BYTES:
        kind, length = struct.unpack_from("<II", raw, offset)
        if kind == 0xFFFFFFFF:
            return lines, offset
        if length < 16 or offset + length > RECORD_BYTES:
            return None
        non_resident = raw[offset + 8]
        name_length = raw[offset + 9]
        name_offset, flags, ident = struct.unpack_from("<HHH", raw, offset + 10)
        common = ["id=%d" % ident]
        if flags:
            common.append("flags=" + number(flags))
        name = b""
        if name_length:
            name = raw[offset + name_offset : offset + name_offset + 2 * name_length]
            common += name_fields("aname", name)
        if non_resident:
            usual = 0x40 + (8 if raw[offset + 34] else 0) if name_length else 0x40
        else:
            usual = 0x18
        if name_offset != usual:
            common.append("noff=%d" % name_offset)
        if non_resident:
            start, last, runs_offset, unit = struct.unpack_from("<QQHH", raw, offset + 16)
            alloc, real, init = struct.unpack_from("<QQQ", raw, offset + 40)
            words = ["nonres", "type=" + number(kind)] + common
            words += ["start=%d" % start, "last=%d" % last,
                      "alloc=%d" % alloc, "real=%d" % real, "init=%d" % init]
            if unit:
                words.append("unit=%d" % unit)
                words.append("csize=%d" % u64(raw, offset + 64))
            position = offset + runs_offset
            while position < offset + length and raw[position] != 0:
                position += 1 + (raw[position] & 15) + (raw[position] >> 4)
            if position >= offset + length:
                return None
            words.append("runs=" + raw[offset + runs_offset : position + 1].hex())
            lines.append([" ".join(words)])
        else:
            value_length, value_offset = struct.unpack_from("<IH", raw, offset + 16)
            indexed = raw[offset + 22]
            value = raw[offset + value_offset : offset + value_offset + value_length]
            lines.append(resident_lines(kind, common, indexed, value, name))
        offset += length
    return None


def resident_lines(kind, common, indexed, value, name):
    """The lines of one resident attribute: the typed line, else a generic one."""
    typed = None
    expected_indexed = 1 if kind == 0x30 else 0
    extra = [] if indexed == expected_indexed else ["indexed=%d" % indexed]
    if kind == 0x10:
        fields = si_fields(value)
        typed = fields and ["si"] + common + extra + fields
    elif kind == 0x30:
        fields = fn_fields(value)
        typed = fields and ["fn"] + common + extra + fields
    elif kind == 0x90 and len(value) >= 32:
        header = struct.unpack_from("<IIIB", value, 0)
        entry_offset, total, alloc, children = struct.unpack_from("<IIIB", value, 16)
        entries = entry_lines(value, 16 + entry_offset, 16 + total)
        if header[0] == 0x30 and entry_offset == 16 and entries is not None:
            words = ["indexroot"] + common + extra
            words += ["rule=%d" % header[1], "block=%d" % header[2],
                      "clusters=%d" % header[3], "children=%d" % children]
            if alloc != total:
                words.append("alloc=%d" % alloc)
            return [" ".join(words)] + entries
    if typed:
        return [" ".join(typed)]
    return [" ".join(["attr", "type=" + number(kind)] + common + extra
                     + ["value=" + value.hex()])]


def parse_record(real, address):
    """(shape, number, attribute lines) of the file record, or None."""
    if real[:4] != b"FILE":
        return None
    array_offset, array_words = struct.unpack_from("<HH", real, 4)
    if array_offset != 0x30 or array_words != 1 + RECORD_BYTES // SECTOR_BYTES:
        return None
    raw = bytearray(real)
    usn = u16(raw, array_offset)
    for sector in range(1, array_words):
        end = sector * SECTOR_BYTES
        if u16(raw, end - 2) != usn:
            return None
        raw[end - 2 : end] = raw[array_offset + 2 * sector : array_offset + 2 * sector + 2]
    (lsn, seq, links, first, flags, used, alloc, base, next_id) = struct.unpack_from(
        "<QHHHHIIQH", raw, 8)
    ident = u32(raw, 44)
    if first != 0x38 or alloc != RECORD_BYTES or u16(raw, 42) != 0:
        return None
    walked = attribute_lines(raw, first)
    if walked is None:
        return None
    attrs, end = walked
    if used != end + 8:
        return None
    seq_word = "seq=@number" if seq == ident else "seq=%d" % seq
    words = ["flags=" + number(flags), seq_word, "links=%d" % links,
             "lsn=%d" % lsn, "usn=%d" % usn, "next_id=%d" % next_id]
    if base:
        words.append("base=" + number(base))
    padding = u32(raw, end + 4)
    if padding:
        words.append("pad=" + number(padding))
    return " ".join(words), ident, [line for attr in attrs for line in attr]


def record_group_lines(group):
    """The layers lines of consecutive records with the same shape."""
    address, shape, ident, attrs, count = group
    head = "record %d count=%d number=%d %s" % (address, count, ident, shape)
    return [head] + attrs


def find_records(volume):
    """Layers lines for the file records that the reads cover."""
    groups = []
    for start, end in volume.intervals:
        for address in range((start + RECORD_BYTES - 1) // RECORD_BYTES * RECORD_BYTES,
                             end - RECORD_BYTES + 1, RECORD_BYTES):
            real = volume.get(address, RECORD_BYTES)
            parsed = parse_record(real, address)
            if parsed is None:
                continue
            shape, ident, attrs = parsed
            if groups:
                last = groups[-1]
                joins = (last[1] == shape and last[3] == attrs
                         and last[0] + last[4] * RECORD_BYTES == address
                         and last[2] + last[4] == ident)
                if joins:
                    last[4] += 1
                    continue
            groups.append([address, shape, ident, attrs, 1])
    lines = []
    for group in groups:
        lines += record_group_lines(group)
    return lines


def find_index_blocks(volume):
    """Layers lines for the index blocks that the reads cover."""
    lines = []
    for start, end in volume.intervals:
        for address in range((start + SECTOR_BYTES - 1) // SECTOR_BYTES * SECTOR_BYTES,
                             end - INDEX_BLOCK_BYTES + 1, SECTOR_BYTES):
            real = volume.get(address, INDEX_BLOCK_BYTES)
            if real[:4] != b"INDX" or u16(real, 4) != 0x28:
                continue
            if u16(real, 6) != 1 + INDEX_BLOCK_BYTES // SECTOR_BYTES:
                continue
            raw = bytearray(real)
            usn = u16(raw, 0x28)
            ok = True
            for sector in range(1, INDEX_BLOCK_BYTES // SECTOR_BYTES + 1):
                tail = sector * SECTOR_BYTES
                if u16(raw, tail - 2) != usn:
                    ok = False
                    break
                raw[tail - 2 : tail] = raw[0x28 + 2 * sector : 0x2A + 2 * sector]
            if not ok:
                continue
            lsn, vcn = struct.unpack_from("<QQ", raw, 8)
            entry_offset, total, alloc, not_leaf = struct.unpack_from("<IIIB", raw, 0x18)
            entries = entry_lines(raw, 0x18 + entry_offset, 0x18 + total)
            if entries is None:
                continue
            words = ["indx %d" % address, "size=%d" % INDEX_BLOCK_BYTES,
                     "vcn=%d" % vcn, "lsn=%d" % lsn, "usn=%d" % usn,
                     "not_leaf=%d" % not_leaf]
            if entry_offset != 0x40:
                words.append("entry_offset=%d" % entry_offset)
            if alloc != INDEX_BLOCK_BYTES - 0x18:
                words.append("alloc=%d" % alloc)
            lines += [" ".join(words)] + entries
    return lines


def counter_text(size):
    """The first size bytes of the text "This is line N." of the log file."""
    out = bytearray()
    number = 0
    while len(out) < size:
        out += b"This is line %d.\r\n" % number
        number += 1
    return bytes(out[:size])


def sector_index(sources):
    """{sector: [(source, offset)]} for the sectors that the sources hold."""
    index = {}
    for key, data in sources:
        for at in range(0, len(data) - SECTOR_BYTES + 1, SECTOR_BYTES):
            sector = data[at : at + SECTOR_BYTES]
            if any(sector):
                index.setdefault(sector, []).append((key, at))
    return index


def lznt1_plain(raw):
    """(plain bytes, chunk bytes) of the LZNT1 chunk at raw[0], or None."""
    header = u16(raw, 0)
    size = (header & 0x0FFF) + 1
    if header & 0x7000 != 0x3000 or size + 2 > len(raw):
        return None
    body = raw[2 : 2 + size]
    if not header & 0x8000:
        return bytes(body), size + 2
    plain = bytearray()
    position = 0
    while position < len(body):
        flags = body[position]
        position += 1
        for bit in range(8):
            if position >= len(body):
                break
            if flags >> bit & 1:
                if position + 2 > len(body) or not plain:
                    return None
                token = u16(body, position)
                position += 2
                offset_bits = 4
                while 1 << offset_bits < len(plain):
                    offset_bits += 1
                length = (token & ((1 << (16 - offset_bits)) - 1)) + 3
                distance = (token >> (16 - offset_bits)) + 1
                if distance > len(plain):
                    return None
                for _ in range(length):
                    plain.append(plain[-distance])
            else:
                plain.append(body[position])
                position += 1
    return bytes(plain), size + 2


def find_compressed_chunks(volume, index, sources, stem):
    """Layers lines for the LZNT1 streams of the files that code makes."""
    data = dict(sources)
    lines = []
    for start, end in volume.intervals:
        address = (start + SECTOR_BYTES - 1) // SECTOR_BYTES * SECTOR_BYTES
        while address + 2 <= end:
            here = address
            address += SECTOR_BYTES
            raw = volume.get(here, min(end - here, 4098))
            found = lznt1_plain(raw) if raw[1] & 0x70 == 0x30 else None
            if found is None or len(found[0]) != CHUNK_BYTES:
                continue
            plain, length = found
            origin = None
            for key, at in index.get(plain[:SECTOR_BYTES], []):
                if at % CHUNK_BYTES == 0 and plain == data[key][at : at + CHUNK_BYTES]:
                    origin = (key, at)
                    break
            if origin is None:
                continue
            key, at = origin
            count = 1
            cursor = here + length
            while cursor + 2 <= end:
                nxt = volume.get(cursor, min(end - cursor, 4098))
                more = lznt1_plain(nxt) if nxt[1] & 0x70 == 0x30 else None
                wanted = data[key][at + count * CHUNK_BYTES : at + (count + 1) * CHUNK_BYTES]
                if more is None or more[0] != wanted:
                    break
                count += 1
                cursor += more[1]
            words = ["lznt1 %d" % here, "file=%d" % at, "count=%d" % count]
            if key is not None:
                words.append("name=%s/%s" % (stem, key))
            lines.append(" ".join(words))
            address = (cursor + 2 + SECTOR_BYTES - 1) // SECTOR_BYTES * SECTOR_BYTES
    return lines


def find_file_data(volume, image, offset, stem, resources, members):
    """Layers lines for the sectors that hold the bytes of a known file."""
    blobs, log_size = FILE_DATA[stem]
    sources = []
    for name, address, length in blobs:
        image.seek(offset + address)
        data = image.read(length)
        members[name] = data
        (resources / stem / name).write_bytes(data)
        sources.append((name, data))
    text = counter_text(log_size)
    sources.append((None, text))
    index = sector_index(sources)
    extents = []
    for start, end in volume.intervals:
        first = (start + SECTOR_BYTES - 1) // SECTOR_BYTES * SECTOR_BYTES
        for address in range(first, end - SECTOR_BYTES + 1, SECTOR_BYTES):
            at = address - start
            found = index.get(volume.data[start][at : at + SECTOR_BYTES])
            if found is None:
                continue
            last = extents[-1] if extents else None
            # A sector that several places hold goes where the extent goes on.
            if last and last[1] + last[3] == address and (
                    last[0], last[2] + last[3]) in found:
                last[3] += SECTOR_BYTES
            else:
                extents.append([found[0][0], address, found[0][1],
                                SECTOR_BYTES])
    lines = []
    for name, address, at, length in extents:
        if name is None:
            lines.append("text %d %d file=%d" % (address, length, at))
        else:
            lines.append("blob %d name=%s/%s offset=%d length=%d"
                         % (address, stem, name, at, length))
    return lines + find_compressed_chunks(volume, index, sources, stem)


def find_boot(volume, stem):
    """(layers line, boot code) for the boot area, or None."""
    real = volume.get(0, BOOT_AREA_BYTES)
    if real is None or real[3:11] != b"NTFS    ":
        return None
    words = ["boot 0", "code=%s/%s" % (stem, BOOT_CODE_NAME),
             "bps=%d" % u16(real, 11), "spc=%d" % real[13],
             "reserved=%d" % u16(real, 14), "media=" + number(real[21]),
             "spt=%d" % u16(real, 24), "heads=%d" % u16(real, 26),
             "hidden=%d" % u32(real, 28), "total=%d" % u64(real, 40),
             "mft=%d" % u64(real, 48), "mirror=%d" % u64(real, 56),
             "record=%d" % u32(real, 64), "index=%d" % u32(real, 68),
             "serial=" + real[72:80].hex()]
    if u32(real, 36) != DEFAULT_UNUSED_MARKER:
        words.append("unused=" + number(u32(real, 36)))
    return " ".join(words), real[BOOT_FIELDS_BYTES:]


def patches(real, expected, start):
    """The patch records for the bytes of one interval that differ."""
    out = bytearray()
    total = len(real)
    position = 0
    while position < total:
        block = min(SKIP_BLOCK, total - position)
        if real[position : position + block] == expected[position : position + block]:
            position += block
            continue
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


def read_patches(residue):
    """[(address, bytes)] of a residue."""
    rows = []
    position = 0
    while position < len(residue):
        address, length = struct.unpack_from("<QI", residue, position)
        position += 12
        rows.append((address, residue[position : position + length]))
        position += length
    return rows


def find_stamps(residue):
    """Stamp lines for the patches that repeat at a fixed distance."""
    by_bytes = {}
    for address, data in read_patches(residue):
        by_bytes.setdefault(data, []).append(address)
    lines = []
    for data, addresses in by_bytes.items():
        addresses.sort()
        first = 0
        while first < len(addresses):
            last = first + 1
            if last < len(addresses):
                stride = addresses[last] - addresses[first]
                while (last + 1 < len(addresses)
                       and addresses[last + 1] - addresses[last] == stride):
                    last += 1
                count = last - first + 1
                if count >= STAMP_MIN_COUNT and stride >= len(data):
                    lines.append("stamp %d stride=%d count=%d bytes=%s"
                                 % (addresses[first], stride, count, data.hex()))
                    first = last + 1
                    continue
            first += 1
    return sorted(lines)


def predict(generator, layers, volume, resources):
    """What NtfsFuzzGen gives for the intervals, as {start: bytes}."""
    with tempfile.TemporaryDirectory() as folder:
        folder = Path(folder)
        (folder / "layers").write_bytes(layers)
        (folder / "intervals").write_text(
            "".join("%d %d\n" % (start, end) for start, end in volume.intervals))
        subprocess.run([str(generator), "--predict", str(folder / "layers"),
                        str(folder / "intervals"), str(folder / "out"),
                        "--resources", str(resources)], check=True)
        joined = (folder / "out").read_bytes()
    out = {}
    position = 0
    for start, end in volume.intervals:
        out[start] = joined[position : position + end - start]
        position += end - start
    return out


def make_residue(volume, expected):
    out = bytearray()
    for start, end in volume.intervals:
        out += patches(volume.data[start], expected[start], start)
    return bytes(out)


def make_layers(layers):
    return "".join(layer if isinstance(layer, str) else "%s %d %d" % layer
                   for layer in layers).encode("ascii")


def layer_text(lines):
    return "".join(line + "\n" for line in lines).encode("ascii")


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


def make_trace(args, stem, members, trace, image_path, offset, layers,
               structure):
    """Makes the layers and the residue of one trace. Returns a report line."""
    intervals = read_intervals(trace)
    lines = ["%s %d %d" % layer for layer in layers]
    with open(args.images / image_path, "rb") as image:
        volume = Volume(image, offset, intervals)
    with tempfile.TemporaryDirectory() as folder:
        resources = Path(folder)
        (resources / stem).mkdir()
        boot = find_boot(volume, stem) if structure else None
        if boot:
            lines.append(boot[0])
            (resources / stem / BOOT_CODE_NAME).write_bytes(boot[1])
            previous = members.get(BOOT_CODE_NAME)
            if previous not in (None, boot[1]):
                raise RuntimeError("the boot code differs from the shared one")
            members[BOOT_CODE_NAME] = boot[1]
        if structure:
            lines += find_records(volume) + find_index_blocks(volume)
        if structure and stem in FILE_DATA:
            with open(args.images / image_path, "rb") as image:
                lines += find_file_data(volume, image, offset, stem, resources,
                                        members)
        text = layer_text(lines)
        expected = predict(args.generator, text, volume, resources)
        residue = make_residue(volume, expected)
        if structure:
            stamps = find_stamps(residue)
            if stamps:
                text = layer_text(lines + stamps)
                expected = predict(args.generator, text, volume, resources)
                residue = make_residue(volume, expected)
    members[trace + ".layers"] = text
    members[trace + ".residue"] = residue
    union = sum(end - start for start, end in intervals)
    patch_bytes = sum(len(data) for _, data in read_patches(residue))
    return ("%s: read set %d bytes, residue %d bytes in %d patches (%d with "
            "headers), layers file %d bytes"
            % (trace, union, patch_bytes, len(read_patches(residue)),
               len(residue), len(text)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--images", type=Path, required=True,
                        help="folder of the forensic images")
    parser.add_argument("--generator", type=Path, required=True,
                        help="the NtfsFuzzGen executable")
    parser.add_argument("--only", nargs="*", default=[],
                        help="traces to make again (default: all)")
    args = parser.parse_args()

    members = {}
    for stem in sorted({row[0] for row in TRACES}):
        members[stem] = old_members(HERE / (stem + ".tar.xz"))
    for stem, trace, image_path, offset, layers, structure in TRACES:
        if args.only and trace not in args.only:
            continue
        print(make_trace(args, stem, members[stem], trace, image_path, offset,
                         layers, structure))
    for stem, files in members.items():
        archive = HERE / (stem + ".tar.xz")
        write_archive(archive, files)
        print("%s: %d bytes" % (archive.name, archive.stat().st_size))


if __name__ == "__main__":
    sys.exit(main())
