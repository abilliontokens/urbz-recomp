#!/usr/bin/env python3
"""XDVDFS (original Xbox / Xbox 360 disc image) reader and extractor.

Stdlib only. Streams file data, never loads a whole file into memory.

    python tools/xiso_extract.py list    <iso>
    python tools/xiso_extract.py extract <iso> <outdir> [--only PATH] [--force]
    python tools/xiso_extract.py info    <iso>
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from dataclasses import dataclass
from typing import Iterator

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
DESCRIPTOR_SECTOR = 32
ATTR_DIRECTORY = 0x10
CHUNK = 1 << 20

# Known game-partition base offsets: trimmed image, XGD1 video+game layouts, XGD2/XGD3.
CANDIDATE_BASES = (
    0x00000000,
    0x0000FD90000,
    0x00018300000,
    0x00002080000,
    0x000001FB20000,
)
SCAN_LIMIT = 0x1A000000  # bytes of the image scanned when no candidate base matches


class XisoError(Exception):
    pass


@dataclass(frozen=True)
class Entry:
    path: str
    sector: int
    size: int
    attributes: int

    @property
    def is_dir(self) -> bool:
        return bool(self.attributes & ATTR_DIRECTORY)

    @property
    def offset(self) -> int:
        return self.sector * SECTOR


class Xiso:
    """Read-only view of the XDVDFS volume inside a disc image."""

    def __init__(self, path: str) -> None:
        self.path = path
        self.image_size = os.path.getsize(path)
        self._fh = open(path, "rb")
        try:
            self.base = self._find_base()
            self.root_sector, self.root_size, self.timestamp = self._read_descriptor()
        except BaseException:
            self._fh.close()
            raise

    # -- lifetime ---------------------------------------------------------
    def close(self) -> None:
        self._fh.close()

    def __enter__(self) -> "Xiso":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- volume -----------------------------------------------------------
    def _has_magic(self, base: int) -> bool:
        off = base + DESCRIPTOR_SECTOR * SECTOR
        if off + SECTOR > self.image_size:
            return False
        self._fh.seek(off)
        sector = self._fh.read(SECTOR)
        return len(sector) == SECTOR and sector[:20] == MAGIC and sector[0x7EC:0x800] == MAGIC

    def _find_base(self) -> int:
        for base in CANDIDATE_BASES:
            if self._has_magic(base):
                return base
        limit = min(self.image_size, SCAN_LIMIT)
        self._fh.seek(0)
        pos = 0
        while pos < limit:
            block = self._fh.read(min(CHUNK, limit - pos))
            if not block:
                break
            start = 0
            while True:
                hit = block.find(MAGIC, start)
                if hit < 0:
                    break
                abs_off = pos + hit
                if abs_off % SECTOR == 0:
                    base = abs_off - DESCRIPTOR_SECTOR * SECTOR
                    if base >= 0 and self._has_magic(base):
                        return base
                start = hit + 1
            pos += len(block)
            self._fh.seek(pos)
        raise XisoError(
            f"{self.path}: no XDVDFS volume descriptor found "
            f"(magic {MAGIC.decode()} absent at sector 32 of every candidate partition base)"
        )

    def _read_descriptor(self) -> tuple[int, int, int]:
        self._fh.seek(self.base + DESCRIPTOR_SECTOR * SECTOR + 20)
        root_sector, root_size, timestamp = struct.unpack("<IIQ", self._fh.read(16))
        if root_size % 4:
            raise XisoError(f"{self.path}: root directory size {root_size} is not 4-byte aligned")
        return root_sector, root_size, timestamp

    # -- directory tree ---------------------------------------------------
    def _read_directory(self, sector: int, size: int, prefix: str) -> list[Entry]:
        """Decode one directory table. Entries form an AVL tree keyed by name;
        links are 4-byte-unit offsets inside the table."""
        if size == 0:
            return []
        off = self.base + sector * SECTOR
        if off + size > self.image_size:
            raise XisoError(f"directory at sector {sector} (size {size}) runs past the end of the image")
        self._fh.seek(off)
        table = self._fh.read(size)
        if len(table) != size:
            raise XisoError(f"short read for directory at sector {sector}")

        out: list[Entry] = []
        seen: set[int] = set()
        stack = [0]
        while stack:
            pos = stack.pop() * 4
            if pos in seen:
                continue
            seen.add(pos)
            if pos + 14 > len(table):
                raise XisoError(f"directory entry at +{pos} runs past the table (sector {sector})")
            left, right, ent_sector, ent_size, attributes, name_len = struct.unpack_from(
                "<HHIIBB", table, pos
            )
            end = pos + 14 + name_len
            if end > len(table):
                raise XisoError(f"directory entry name at +{pos} runs past the table (sector {sector})")
            name = table[pos + 14 : end].decode("latin-1")
            if name not in ("", ".", ".."):
                out.append(
                    Entry(
                        path=f"{prefix}{name}",
                        sector=ent_sector,
                        size=ent_size,
                        attributes=attributes,
                    )
                )
            for link in (left, right):
                if link not in (0, 0xFFFF):
                    stack.append(link)
        return out

    def walk(self) -> Iterator[Entry]:
        """Yield every entry depth-first, directories before their contents."""
        pending = [(self.root_sector, self.root_size, "")]
        guard: set[tuple[int, int, str]] = set()
        while pending:
            sector, size, prefix = pending.pop()
            key = (sector, size, prefix)
            if key in guard:
                continue
            guard.add(key)
            for entry in sorted(self._read_directory(sector, size, prefix), key=lambda e: e.path):
                yield entry
                if entry.is_dir:
                    pending.append((entry.sector, entry.size, f"{entry.path}/"))

    def files(self) -> list[Entry]:
        return sorted((e for e in self.walk() if not e.is_dir), key=lambda e: e.path.lower())

    # -- data -------------------------------------------------------------
    def stream(self, entry: Entry) -> Iterator[bytes]:
        if entry.size == 0:
            return
        start = self.base + entry.offset
        if start + entry.size > self.image_size:
            raise XisoError(f"{entry.path}: data runs past the end of the image")
        self._fh.seek(start)
        left = entry.size
        while left:
            block = self._fh.read(min(CHUNK, left))
            if not block:
                raise XisoError(f"{entry.path}: short read with {left} bytes left")
            left -= len(block)
            yield block

    def extract(self, entry: Entry, outdir: str, force: bool) -> int:
        dest = safe_join(outdir, entry.path)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.exists(dest) and not force and os.path.getsize(dest) == entry.size:
            return 0
        written = 0
        with open(dest, "wb") as fh:
            for block in self.stream(entry):
                fh.write(block)
                written += len(block)
        return written


def safe_join(outdir: str, rel: str) -> str:
    root = os.path.abspath(outdir)
    dest = os.path.abspath(os.path.join(root, *rel.split("/")))
    if dest != root and not dest.startswith(root + os.sep):
        raise XisoError(f"entry {rel!r} escapes the output directory")
    return dest


def human(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0
    return str(n)


def cmd_info(args: argparse.Namespace) -> int:
    with Xiso(args.iso) as iso:
        files = iso.files()
        total = sum(e.size for e in files)
        print(f"image            {iso.path}")
        print(f"image size       {iso.image_size} bytes")
        print(f"partition base   0x{iso.base:08X}")
        print(f"root sector      {iso.root_sector} (offset 0x{iso.base + iso.root_sector * SECTOR:X})")
        print(f"root table size  {iso.root_size} bytes")
        print(f"volume filetime  0x{iso.timestamp:016X}")
        print(f"files            {len(files)}")
        print(f"file bytes       {total} ({human(total)})")
    return 0


def cmd_list(args: argparse.Namespace) -> int:
    with Xiso(args.iso) as iso:
        files = iso.files()
        for entry in files:
            print(f"{entry.path}\t{entry.size}")
        total = sum(e.size for e in files)
        print(f"# {len(files)} files, {total} bytes ({human(total)}), partition base 0x{iso.base:08X}")
    return 0


def cmd_extract(args: argparse.Namespace) -> int:
    with Xiso(args.iso) as iso:
        files = iso.files()
        if args.only:
            wanted = args.only.replace("\\", "/").lstrip("/").lower()
            files = [e for e in files if e.path.lower() == wanted]
            if not files:
                print(f"no such file in image: {args.only}", file=sys.stderr)
                return 2
        os.makedirs(args.outdir, exist_ok=True)
        written = 0
        for index, entry in enumerate(files, 1):
            written += iso.extract(entry, args.outdir, args.force)
            if index % 200 == 0 or index == len(files):
                print(f"  {index}/{len(files)} files, {human(written)} written", flush=True)
        print(f"extracted {len(files)} files, {written} bytes ({human(written)}) to {args.outdir}")
        xbe = safe_join(args.outdir, "default.xbe")
        if os.path.exists(xbe):
            with open(xbe, "rb") as fh:
                head = fh.read(4)
                fh.seek(0)
                digest = hashlib.sha1()
                for block in iter(lambda: fh.read(CHUNK), b""):
                    digest.update(block)
            print(f"default.xbe      {os.path.getsize(xbe)} bytes, magic {head!r}, sha1 {digest.hexdigest()}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="XDVDFS (Xbox disc image) reader")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_info = sub.add_parser("info", help="print volume geometry and totals")
    p_info.add_argument("iso")
    p_info.set_defaults(func=cmd_info)

    p_list = sub.add_parser("list", help="print every file as path<TAB>size")
    p_list.add_argument("iso")
    p_list.set_defaults(func=cmd_list)

    p_ext = sub.add_parser("extract", help="extract the image into a directory")
    p_ext.add_argument("iso")
    p_ext.add_argument("outdir")
    p_ext.add_argument("--only", help="extract a single file, e.g. default.xbe")
    p_ext.add_argument("--force", action="store_true", help="rewrite files that already match in size")
    p_ext.set_defaults(func=cmd_extract)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except XisoError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
