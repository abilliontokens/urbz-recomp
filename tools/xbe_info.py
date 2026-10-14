#!/usr/bin/env python3
"""XBE (original Xbox executable) reader: header, certificate, sections,
library versions, TLS and the kernel import thunk table.

Kernel ordinals are resolved through the SDK's own name table
(`sdk/runtime/kernel/ordinals.cpp`) so the report names exactly what the host
runtime has to answer for. Stdlib only.

    python tools/xbe_info.py assets/default.xbe
    python tools/xbe_info.py assets/default.xbe --json build/xbe_info.json
    python tools/xbe_info.py assets/default.xbe --markdown docs/xbe-analysis.md
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import struct
import sys

# Entry point and kernel thunk are stored XOR'd; the key identifies the build type.
ENTRY_KEYS = {"retail": 0xA8FC57AB, "debug": 0x94859D4B, "chihiro": 0x40B5C16E}
THUNK_KEYS = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152, "chihiro": 0x2290059D}

SECTION_FLAGS = [
    (0x00000001, "writable"),
    (0x00000002, "preload"),
    (0x00000004, "executable"),
    (0x00000008, "inserted_file"),
    (0x00000010, "head_page_read_only"),
    (0x00000020, "tail_page_read_only"),
]

INIT_FLAGS = [
    (0x00000001, "mount_utility_drive"),
    (0x00000002, "format_utility_drive"),
    (0x00000004, "limit_64mb"),
    (0x00000008, "dont_setup_harddisk"),
]

MEDIA_FLAGS = [
    (0x00000001, "hard_disk"),
    (0x00000002, "dvd_x2"),
    (0x00000004, "dvd_cd"),
    (0x00000008, "cd"),
    (0x00000010, "dvd_5_ro"),
    (0x00000020, "dvd_9_ro"),
    (0x00000040, "dvd_5_rw"),
    (0x00000080, "dvd_9_rw"),
    (0x00000100, "dongle"),
    (0x00000200, "media_board"),
    (0x40000000, "nonsecure_hard_disk"),
    (0x80000000, "nonsecure_mode"),
]

REGIONS = [
    (0x00000001, "north_america"),
    (0x00000002, "japan"),
    (0x00000004, "rest_of_world"),
    (0x80000000, "manufacturing"),
]

LIBRARY_FLAGS_APPROVED = {0: "unapproved", 1: "possibly_approved", 2: "approved", 3: "approved"}


class XbeError(Exception):
    pass


def flag_names(value: int, table: list[tuple[int, str]]) -> list[str]:
    return [name for bit, name in table if value & bit]


def filetime(value: int) -> str:
    if not value:
        return "0"
    epoch = dt.datetime(1601, 1, 1, tzinfo=dt.timezone.utc)
    return (epoch + dt.timedelta(microseconds=value // 10)).isoformat()


def unixtime(value: int) -> str:
    if not value:
        return "0"
    return dt.datetime.fromtimestamp(value, dt.timezone.utc).isoformat()


class Xbe:
    def __init__(self, path: str) -> None:
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        if self.data[:4] != b"XBEH":
            raise XbeError(f"{path}: not an XBE (magic {self.data[:4]!r})")
        self.sha1 = hashlib.sha1(self.data).hexdigest()
        self._parse_header()
        self._parse_certificate()
        self._parse_sections()
        self._parse_libraries()
        self._parse_tls()
        self._parse_kernel_imports()

    # -- helpers ----------------------------------------------------------
    def u32(self, off: int) -> int:
        return struct.unpack_from("<I", self.data, off)[0]

    def file_off(self, va: int) -> int:
        """Translate a virtual address to a file offset using the section table."""
        if va == 0:
            raise XbeError("virtual address 0 has no file offset")
        if self.base <= va < self.base + self.size_of_headers:
            return va - self.base
        for sec in self.sections:
            if sec["virtual_addr"] <= va < sec["virtual_addr"] + max(sec["virtual_size"], sec["raw_size"]):
                delta = va - sec["virtual_addr"]
                if delta >= sec["raw_size"]:
                    raise XbeError(f"virtual address 0x{va:08X} is in uninitialised part of {sec['name']}")
                return sec["raw_addr"] + delta
        raise XbeError(f"virtual address 0x{va:08X} is outside every section")

    def cstring(self, va: int) -> str:
        off = self.file_off(va)
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("latin-1")

    # -- structures -------------------------------------------------------
    def _parse_header(self) -> None:
        d = self.data
        self.base = self.u32(0x104)
        self.size_of_headers = self.u32(0x108)
        self.size_of_image = self.u32(0x10C)
        self.size_of_image_header = self.u32(0x110)
        self.timestamp = self.u32(0x114)
        self.certificate_addr = self.u32(0x118)
        self.section_count = self.u32(0x11C)
        self.section_headers_addr = self.u32(0x120)
        self.init_flags = self.u32(0x124)
        self.entry_raw = self.u32(0x128)
        self.tls_addr = self.u32(0x12C)
        self.pe_stack_commit = self.u32(0x130)
        self.pe_heap_reserve = self.u32(0x134)
        self.pe_heap_commit = self.u32(0x138)
        self.pe_base_addr = self.u32(0x13C)
        self.pe_size_of_image = self.u32(0x140)
        self.pe_checksum = self.u32(0x144)
        self.pe_timestamp = self.u32(0x148)
        self.debug_pathname_addr = self.u32(0x14C)
        self.debug_filename_addr = self.u32(0x150)
        self.debug_unicode_filename_addr = self.u32(0x154)
        self.thunk_raw = self.u32(0x158)
        self.non_kernel_import_dir_addr = self.u32(0x15C)
        self.library_version_count = self.u32(0x160)
        self.library_versions_addr = self.u32(0x164)
        self.kernel_library_version_addr = self.u32(0x168)
        self.xapi_library_version_addr = self.u32(0x16C)
        self.logo_bitmap_addr = self.u32(0x170)
        self.logo_bitmap_size = self.u32(0x174)

        # The key that lands the entry point inside the image identifies the build.
        self.build_type = "unknown"
        self.entry_point = self.entry_raw
        for name, key in ENTRY_KEYS.items():
            candidate = self.entry_raw ^ key
            if self.base <= candidate < self.base + self.size_of_image:
                self.build_type, self.entry_point = name, candidate
                break
        self.kernel_thunk_addr = self.thunk_raw ^ THUNK_KEYS.get(self.build_type, 0)
        del d

    def _parse_certificate(self) -> None:
        off = self.file_off(self.certificate_addr)
        size, cert_time, title_id = struct.unpack_from("<III", self.data, off)
        name = self.data[off + 0x0C : off + 0x5C].decode("utf-16-le").rstrip("\0")
        alt_ids = [v for v in struct.unpack_from("<16I", self.data, off + 0x5C) if v]
        media, region, ratings, disc, version = struct.unpack_from("<5I", self.data, off + 0x9C)
        self.certificate = {
            "size": size,
            "timestamp": cert_time,
            "timestamp_iso": unixtime(cert_time),
            "title_id": title_id,
            "title_id_hex": f"0x{title_id:08X}",
            "publisher": chr((title_id >> 24) & 0xFF) + chr((title_id >> 16) & 0xFF),
            "serial": title_id & 0xFFFF,
            "title_name": name,
            "alt_title_ids": [f"0x{v:08X}" for v in alt_ids],
            "allowed_media": f"0x{media:08X}",
            "allowed_media_flags": flag_names(media, MEDIA_FLAGS),
            "game_region": f"0x{region:08X}",
            "game_region_flags": flag_names(region, REGIONS),
            "game_ratings": f"0x{ratings:08X}",
            "disk_number": disc,
            "version": f"0x{version:08X}",
        }

    def _parse_sections(self) -> None:
        self.sections = []
        base = self.file_off(self.section_headers_addr)
        for i in range(self.section_count):
            off = base + i * 0x38
            (
                flags,
                vaddr,
                vsize,
                raddr,
                rsize,
                name_addr,
                name_refs,
                head_refs,
                tail_refs,
            ) = struct.unpack_from("<9I", self.data, off)
            digest = self.data[off + 0x24 : off + 0x38].hex()
            self.sections.append(
                {
                    "index": i,
                    "name": "",
                    "name_addr": name_addr,
                    "flags": f"0x{flags:08X}",
                    "flag_names": flag_names(flags, SECTION_FLAGS),
                    "virtual_addr": vaddr,
                    "virtual_size": vsize,
                    "raw_addr": raddr,
                    "raw_size": rsize,
                    "name_ref_count": name_refs,
                    "head_shared_ref_addr": head_refs,
                    "tail_shared_ref_addr": tail_refs,
                    "digest_sha1": digest,
                }
            )
        for sec in self.sections:
            try:
                sec["name"] = self.cstring(sec["name_addr"])
            except (XbeError, ValueError):
                sec["name"] = f"<section{sec['index']}>"
        for sec in self.sections:
            sec["virtual_addr_hex"] = f"0x{sec['virtual_addr']:08X}"
            sec["executable"] = "executable" in sec["flag_names"]

    def _parse_libraries(self) -> None:
        self.libraries = []
        if not self.library_versions_addr:
            return
        base = self.file_off(self.library_versions_addr)
        for i in range(self.library_version_count):
            off = base + i * 0x10
            name = self.data[off : off + 8].decode("latin-1").rstrip("\0")
            major, minor, build, flags = struct.unpack_from("<4H", self.data, off + 8)
            self.libraries.append(
                {
                    "name": name,
                    "major": major,
                    "minor": minor,
                    "build": build,
                    "qfe": flags & 0x1FFF,
                    "approved": LIBRARY_FLAGS_APPROVED.get((flags >> 13) & 0x3, "unknown"),
                    "debug_build": bool(flags & 0x8000),
                    "version": f"{major}.{minor}.{build}",
                }
            )
        builds = {lib["build"] for lib in self.libraries}
        self.xdk_build = max(builds) if builds else 0
        self.xdk_builds = sorted(builds)

    def _parse_tls(self) -> None:
        self.tls = None
        if not self.tls_addr:
            return
        try:
            off = self.file_off(self.tls_addr)
        except XbeError:
            return
        data_start, data_end, index_addr, callback_addr, zero_fill, characteristics = struct.unpack_from(
            "<6I", self.data, off
        )
        self.tls = {
            "address": f"0x{self.tls_addr:08X}",
            "data_start": f"0x{data_start:08X}",
            "data_end": f"0x{data_end:08X}",
            "index_addr": f"0x{index_addr:08X}",
            "callback_addr": f"0x{callback_addr:08X}",
            "zero_fill": zero_fill,
            "characteristics": f"0x{characteristics:08X}",
        }

    def _parse_kernel_imports(self) -> None:
        self.imports: list[dict] = []
        self.import_error = None
        if not self.kernel_thunk_addr:
            self.import_error = "no kernel thunk table"
            return
        try:
            off = self.file_off(self.kernel_thunk_addr)
        except XbeError as exc:
            self.import_error = str(exc)
            return
        va = self.kernel_thunk_addr
        while True:
            value = self.u32(off)
            if value == 0:
                break
            if not value & 0x80000000:
                self.import_error = f"thunk at 0x{va:08X} is not an ordinal import (0x{value:08X})"
                break
            self.imports.append({"ordinal": value & 0xFFFF, "thunk_addr": f"0x{va:08X}"})
            off += 4
            va += 4
            if len(self.imports) > 1024:
                self.import_error = "thunk table did not terminate within 1024 entries"
                break


def load_ordinal_names(sdk_dir: str) -> dict[int, str]:
    """Parse the SDK's kernel ordinal name table (ordinals.cpp)."""
    path = os.path.join(sdk_dir, "runtime", "kernel", "ordinals.cpp")
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    match = re.search(r"kNames\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not match:
        return {}
    names: dict[int, str] = {}
    index = 0
    for token in re.finditer(r'nullptr|"([^"]*)"', match.group(1)):
        if token.group(0) != "nullptr":
            names[index] = token.group(1)
        index += 1
    return names


def sdk_xdk_builds(sdk_dir: str) -> dict[int, dict[str, bool]]:
    root = os.path.join(sdk_dir, "xdk")
    out: dict[int, dict[str, bool]] = {}
    if not os.path.isdir(root):
        return out
    for name in sorted(os.listdir(root)):
        if not name.isdigit():
            continue
        d = os.path.join(root, name)
        out[int(name)] = {
            "layout_toml": os.path.exists(os.path.join(d, "layout.toml")),
            "signatures_toml": os.path.exists(os.path.join(d, "signatures.toml")),
        }
    return out


def build_report(xbe: Xbe, sdk_dir: str) -> dict:
    names = load_ordinal_names(sdk_dir)
    imports = [
        {**imp, "name": names.get(imp["ordinal"], "?"), "known_to_sdk": imp["ordinal"] in names}
        for imp in xbe.imports
    ]
    builds = sdk_xdk_builds(sdk_dir)
    supported = builds.get(xbe.xdk_build, {})
    return {
        "file": os.path.abspath(xbe.path),
        "file_size": len(xbe.data),
        "sha1": xbe.sha1,
        "build_type": xbe.build_type,
        "base_address": f"0x{xbe.base:08X}",
        "entry_point": f"0x{xbe.entry_point:08X}",
        "entry_point_raw": f"0x{xbe.entry_raw:08X}",
        "kernel_thunk_addr": f"0x{xbe.kernel_thunk_addr:08X}",
        "size_of_image": xbe.size_of_image,
        "size_of_headers": xbe.size_of_headers,
        "timestamp": unixtime(xbe.timestamp),
        "pe_timestamp": unixtime(xbe.pe_timestamp),
        "pe_base_address": f"0x{xbe.pe_base_addr:08X}",
        "pe_stack_commit": xbe.pe_stack_commit,
        "pe_heap_reserve": xbe.pe_heap_reserve,
        "pe_heap_commit": xbe.pe_heap_commit,
        "init_flags": f"0x{xbe.init_flags:08X}",
        "init_flag_names": flag_names(xbe.init_flags, INIT_FLAGS),
        "debug_pathname": safe_str(xbe, xbe.debug_pathname_addr),
        "debug_filename": safe_str(xbe, xbe.debug_filename_addr),
        "logo_bitmap_size": xbe.logo_bitmap_size,
        "certificate": xbe.certificate,
        "sections": xbe.sections,
        "code_sections": [s["name"] for s in xbe.sections if s["executable"]],
        "libraries": xbe.libraries,
        "xdk_build": xbe.xdk_build,
        "xdk_builds_present": xbe.xdk_builds,
        "tls": xbe.tls,
        "kernel_imports": imports,
        "kernel_import_count": len(imports),
        "kernel_import_error": xbe.import_error,
        "unresolved_ordinals": sorted({i["ordinal"] for i in imports if not i["known_to_sdk"]}),
        "sdk_xdk_builds": {str(k): v for k, v in builds.items()},
        "sdk_supports_xdk_build": bool(supported.get("signatures_toml")),
        "sdk_has_layout_for_build": bool(supported.get("layout_toml")),
    }


def safe_str(xbe: Xbe, va: int) -> str:
    if not va:
        return ""
    try:
        return xbe.cstring(va)
    except (XbeError, ValueError):
        return ""


def print_report(r: dict) -> None:
    cert = r["certificate"]
    print(f"file              {r['file']}")
    print(f"size / sha1       {r['file_size']} bytes / {r['sha1']}")
    print(f"title             {cert['title_name']}  ({cert['title_id_hex']}, {cert['publisher']}-{cert['serial']})")
    print(f"region / media    {','.join(cert['game_region_flags'])} / {','.join(cert['allowed_media_flags'])}")
    print(f"build type        {r['build_type']}")
    print(f"base / entry      {r['base_address']} / {r['entry_point']}")
    print(f"image size        {r['size_of_image']} bytes, headers {r['size_of_headers']}")
    print(f"built             {r['timestamp']} (cert {cert['timestamp_iso']})")
    print(f"debug name        {r['debug_pathname'] or r['debug_filename']}")
    print(f"kernel thunk      {r['kernel_thunk_addr']}, {r['kernel_import_count']} imports")
    print(f"XDK build         {r['xdk_build']}  (libraries: {', '.join(str(b) for b in r['xdk_builds_present'])})")
    print(
        f"SDK support       signatures.toml={'yes' if r['sdk_supports_xdk_build'] else 'NO'}, "
        f"layout.toml={'yes' if r['sdk_has_layout_for_build'] else 'NO'} "
        f"(SDK has: {', '.join(sorted(r['sdk_xdk_builds'])) or 'none'})"
    )
    print()
    print("sections:")
    print(f"  {'name':<12} {'vaddr':>10} {'vsize':>10} {'raw':>10} {'rawsize':>10}  flags")
    for s in r["sections"]:
        print(
            f"  {s['name']:<12} {s['virtual_addr_hex']:>10} {s['virtual_size']:>10} "
            f"0x{s['raw_addr']:08X} {s['raw_size']:>10}  {','.join(s['flag_names'])}"
        )
    print()
    print("libraries:")
    for lib in r["libraries"]:
        print(
            f"  {lib['name']:<10} {lib['version']:<10} qfe={lib['qfe']:<3} "
            f"{lib['approved']}{' debug' if lib['debug_build'] else ''}"
        )
    print()
    print(f"kernel imports ({r['kernel_import_count']}):")
    for imp in sorted(r["kernel_imports"], key=lambda i: i["ordinal"]):
        print(f"  {imp['ordinal']:>4}  {imp['name']}")
    if r["unresolved_ordinals"]:
        print(f"  ordinals with no name in the SDK table: {r['unresolved_ordinals']}")
    if r["kernel_import_error"]:
        print(f"  note: {r['kernel_import_error']}")


def write_markdown(r: dict, path: str) -> None:
    cert = r["certificate"]
    lines: list[str] = []
    add = lines.append
    add(f"# XBE analysis - {cert['title_name']}")
    add("")
    add(f"Generated by `tools/xbe_info.py` from `{os.path.basename(r['file'])}`.")
    add("")
    add("## Identity")
    add("")
    add("| Field | Value |")
    add("| --- | --- |")
    add(f"| Title | {cert['title_name']} |")
    add(f"| Title ID | {cert['title_id_hex']} ({cert['publisher']}-{cert['serial']}) |")
    add(f"| Region | {', '.join(cert['game_region_flags'])} |")
    add(f"| Allowed media | {', '.join(cert['allowed_media_flags'])} |")
    add(f"| Disc / version | {cert['disk_number']} / {cert['version']} |")
    add(f"| Certificate time | {cert['timestamp_iso']} |")
    add(f"| File size | {r['file_size']} bytes |")
    add(f"| SHA-1 | `{r['sha1']}` |")
    add("")
    add("## Image")
    add("")
    add("| Field | Value |")
    add("| --- | --- |")
    add(f"| Build type (XOR key) | {r['build_type']} |")
    add(f"| Base address | {r['base_address']} |")
    add(f"| Entry point | {r['entry_point']} (stored {r['entry_point_raw']}) |")
    add(f"| Kernel thunk table | {r['kernel_thunk_addr']} |")
    add(f"| Size of image | {r['size_of_image']} bytes |")
    add(f"| Size of headers | {r['size_of_headers']} bytes |")
    add(f"| Linker timestamp | {r['timestamp']} |")
    add(f"| PE timestamp | {r['pe_timestamp']} |")
    add(f"| PE base address | {r['pe_base_address']} |")
    add(f"| Stack commit | {r['pe_stack_commit']} bytes |")
    add(f"| Heap reserve / commit | {r['pe_heap_reserve']} / {r['pe_heap_commit']} bytes |")
    add(f"| Init flags | {r['init_flags']} ({', '.join(r['init_flag_names']) or 'none'}) |")
    add(f"| Debug name | `{r['debug_pathname'] or r['debug_filename']}` |")
    add(f"| Logo bitmap | {r['logo_bitmap_size']} bytes |")
    add(f"| TLS directory | {r['tls']['address'] if r['tls'] else 'none'} |")
    add("")
    add("## Sections")
    add("")
    add("| # | Name | Virtual | Size | Raw offset | Raw size | Flags |")
    add("| --- | --- | --- | --- | --- | --- | --- |")
    for s in r["sections"]:
        add(
            f"| {s['index']} | `{s['name']}` | {s['virtual_addr_hex']} | {s['virtual_size']} | "
            f"0x{s['raw_addr']:08X} | {s['raw_size']} | {', '.join(s['flag_names'])} |"
        )
    add("")
    add(f"Code sections (recompiler input): {', '.join('`' + n + '`' for n in r['code_sections'])}")
    add("")
    add("## XDK libraries")
    add("")
    add("| Library | Version | QFE | Approval | Debug |")
    add("| --- | --- | --- | --- | --- |")
    for lib in r["libraries"]:
        add(f"| `{lib['name']}` | {lib['version']} | {lib['qfe']} | {lib['approved']} | {lib['debug_build']} |")
    add("")
    add(f"XDK build: **{r['xdk_build']}**")
    add("")
    add(
        f"SDK coverage for this build: `signatures.toml` "
        f"{'present' if r['sdk_supports_xdk_build'] else '**missing**'}, `layout.toml` "
        f"{'present' if r['sdk_has_layout_for_build'] else '**missing**'}. "
        f"SDK ships: {', '.join(sorted(r['sdk_xdk_builds'])) or 'none'}."
    )
    add("")
    add(f"## Kernel imports ({r['kernel_import_count']})")
    add("")
    add("Ordinals taken from the XBE thunk table, named with the SDK's `runtime/kernel/ordinals.cpp`.")
    add("")
    add("| Ordinal | Export | Thunk |")
    add("| --- | --- | --- |")
    for imp in sorted(r["kernel_imports"], key=lambda i: i["ordinal"]):
        add(f"| {imp['ordinal']} | `{imp['name']}` | {imp['thunk_addr']} |")
    add("")
    if r["unresolved_ordinals"]:
        add(f"Ordinals the SDK name table does not cover: {r['unresolved_ordinals']}")
        add("")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="XBE header / import reader")
    parser.add_argument("xbe")
    parser.add_argument("--sdk", default="sdk", help="path to the XboxXBERecomp SDK checkout")
    parser.add_argument("--json", help="write the full report as JSON to this path")
    parser.add_argument("--markdown", help="write a markdown report to this path")
    parser.add_argument("--quiet", action="store_true", help="suppress the text report")
    args = parser.parse_args(argv)

    try:
        xbe = Xbe(args.xbe)
    except (XbeError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    report = build_report(xbe, args.sdk)
    if not args.quiet:
        print_report(report)
    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(report, fh, indent=2)
            fh.write("\n")
    if args.markdown:
        os.makedirs(os.path.dirname(os.path.abspath(args.markdown)), exist_ok=True)
        write_markdown(report, args.markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
