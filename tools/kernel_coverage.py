#!/usr/bin/env python3
"""Cross-check the XBE's kernel imports against the exports the SDK runtime registers.

The runtime binds kernel imports by ordinal: `KERNEL_EXPORT(ord, name, args)`,
`KERNEL_FASTCALL(ord, name)` and `KERNEL_DATA(ord, name, size)` fill a 400-slot
table (`sdk/runtime/cpu/dispatch.cpp`), and an import with no entry logs
"kernel import <n> (<name>) has no implementation" at boot and traps if called.
This reports exactly which of *this* game's imports are covered.

    python tools/kernel_coverage.py assets/default.xbe
    python tools/kernel_coverage.py assets/default.xbe --markdown docs/kernel-coverage.md
"""

from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from xbe_info import Xbe, XbeError, load_ordinal_names  # noqa: E402

REGISTRATION = re.compile(
    r"\bKERNEL_(EXPORT|FASTCALL|DATA)\s*\(\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)"
)


def sdk_exports(sdk_dir: str) -> dict[int, tuple[str, str]]:
    """ordinal -> (kind, name) for every export the runtime registers."""
    root = os.path.join(sdk_dir, "runtime", "kernel")
    found: dict[int, tuple[str, str]] = {}
    if not os.path.isdir(root):
        return found
    for name in sorted(os.listdir(root)):
        if not name.endswith((".cpp", ".h")):
            continue
        with open(os.path.join(root, name), "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for kind, ordinal, symbol in REGISTRATION.findall(text):
            if symbol in ("ord", "name"):  # the macro definitions themselves
                continue
            found[int(ordinal, 0)] = (kind.lower(), symbol)
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="kernel import coverage for an XBE")
    parser.add_argument("xbe")
    parser.add_argument("--sdk", default="sdk")
    parser.add_argument("--markdown", help="write the report to this path as well")
    args = parser.parse_args(argv)

    try:
        xbe = Xbe(args.xbe)
    except (XbeError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    names = load_ordinal_names(args.sdk)
    exports = sdk_exports(args.sdk)
    if not exports:
        print(f"error: no kernel export registrations found under {args.sdk}/runtime/kernel", file=sys.stderr)
        return 1

    rows = []
    for imp in sorted(xbe.imports, key=lambda i: i["ordinal"]):
        ordinal = imp["ordinal"]
        kind, symbol = exports.get(ordinal, ("", ""))
        rows.append((ordinal, names.get(ordinal, "?"), kind or "MISSING", symbol))

    covered = [r for r in rows if r[2] != "MISSING"]
    missing = [r for r in rows if r[2] == "MISSING"]

    print(f"{os.path.basename(args.xbe)}: {len(rows)} kernel imports")
    print(f"  implemented by the SDK runtime: {len(covered)}")
    print(f"  not implemented (traps if called): {len(missing)}")
    for ordinal, name, _, _ in missing:
        print(f"    {ordinal:>4}  {name}")
    print(f"  SDK registers {len(exports)} exports in total")

    if args.markdown:
        lines = [
            "# Kernel import coverage",
            "",
            f"`{os.path.basename(args.xbe)}` imports {len(rows)} xboxkrnl ordinals. "
            f"The SDK runtime registers {len(exports)} exports; "
            f"{len(covered)} of this game's imports are covered, {len(missing)} are not.",
            "",
            "An uncovered import logs `boot: kernel import <n> (<name>) has no implementation` at boot",
            "and traps the first time the game calls it.",
            "",
            "| Ordinal | Export | SDK |",
            "| --- | --- | --- |",
        ]
        for ordinal, name, kind, symbol in rows:
            cell = "not implemented" if kind == "MISSING" else f"`{kind}` {symbol}"
            lines.append(f"| {ordinal} | `{name}` | {cell} |")
        lines.append("")
        os.makedirs(os.path.dirname(os.path.abspath(args.markdown)), exist_ok=True)
        with open(args.markdown, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
