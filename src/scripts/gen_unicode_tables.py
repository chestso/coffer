#!/usr/bin/env python3
"""
Generate coffer's Unicode interval tables as a standalone C header.

Emits a data-only header — sorted, non-overlapping {lo, hi} ranges and a
`<PREFIX>NAME_LEN` macro per table — from the Unicode Character Database:

    UAX #11 East Asian Width     classes W/F (wide), class A (ambiguous)
    UAX #29 Grapheme Cluster     GCB Extend, ZWJ, SpacingMark, Prepend
    UTS #51 emoji-data           Extended_Pictographic
    DerivedCoreProperties        Default_Ignorable_Code_Point

The output carries a provenance banner: the Unicode version and SHA-256 of
every UCD file it was built from, the SHA-256 of this generator, and the
exact command line. That is what makes a vendored copy (boba keeps one, to
stay dependency-free) auditable: a stale copy is visible at a glance, and
`--prefix`/`--type` let another project emit the same data under its own
names without linking coffer.

Usage:
    python3 src/scripts/gen_unicode_tables.py --out src/unicode_tables.h

    # Use a local UCD directory instead of downloading:
    python3 src/scripts/gen_unicode_tables.py --ucd /path/to/UCD --out src/unicode_tables.h

    # Another project's vocabulary (boba's sync script):
    python3 .../gen_unicode_tables.py --prefix TU_ --type TuiRange --out src/unicode_tables.h

Rerun when the target Unicode version changes; commit the regenerated
header. No third-party dependencies.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
import urllib.request
from collections import defaultdict
from typing import Iterable

CP_RANGE = re.compile(r"^([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*([^#\s]+)")
UCD_VERSION = re.compile(r"^#\s+(\S+)-(\d+\.\d+\.\d+)\.txt")
UCD_VERSION_LINE = re.compile(r"^#\s+Version:\s*(\d+\.\d+\.\d+)")

UCD_BASE = "https://www.unicode.org/Public/UCD/latest/ucd/"

UCD_FILES = [
    "EastAsianWidth.txt",
    "auxiliary/GraphemeBreakProperty.txt",
    "emoji/emoji-data.txt",
    "DerivedCoreProperties.txt",
]

# coffer's vocabulary: width.c consumes the header as-is.
DEFAULT_PREFIX = "CFR_"
DEFAULT_TYPE = "CfrRange"


def load_ucd(ucd: str) -> dict[str, bytes]:
    """Read the needed UCD files from `ucd`, return {relpath: bytes}."""
    out: dict[str, bytes] = {}
    for relpath in UCD_FILES:
        with open(os.path.join(ucd, relpath), "rb") as f:
            out[relpath] = f.read()
    return out


def download_ucd() -> dict[str, bytes]:
    """Fetch the needed UCD files, return {relpath: bytes}."""
    out: dict[str, bytes] = {}
    for relpath in UCD_FILES:
        url = UCD_BASE + relpath
        print(f"Downloading {url} ...", file=sys.stderr)
        with urllib.request.urlopen(url) as resp:
            out[relpath] = resp.read()
    return out


def ucd_version(data: bytes) -> str:
    for line in data.decode("utf-8", "replace").splitlines()[:12]:
        m = UCD_VERSION.match(line) or UCD_VERSION_LINE.match(line)
        if m:
            return m.group(m.lastindex)
    return "unknown"


def parse_props(data: bytes) -> dict[str, list[tuple[int, int]]]:
    """Parse UCD-format property text into {prop: [(lo, hi), ...]}."""
    out: dict[str, list[tuple[int, int]]] = defaultdict(list)
    for line in data.decode("utf-8", "replace").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        m = CP_RANGE.match(line)
        if not m:
            continue
        lo = int(m.group(1), 16)
        hi = int(m.group(2), 16) if m.group(2) else lo
        out[m.group(3)].append((lo, hi))
    return out


def coalesce(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    """Sort + merge adjacent/overlapping ranges."""
    items = sorted(ranges)
    out: list[tuple[int, int]] = []
    for lo, hi in items:
        if out and lo <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], hi))
        else:
            out.append((lo, hi))
    return out


def emit_table(
    name: str, ranges: list[tuple[int, int]], prefix: str, type_name: str
) -> str:
    lines = [f"{prefix}TABLE_UNUSED static const {type_name} {prefix}{name}[] = {{"]
    for lo, hi in ranges:
        lines.append(f"    {{ 0x{lo:04X}, 0x{hi:04X} }},")
    lines.append("};")
    lines.append(
        f"#define {prefix}{name}_LEN ((sizeof({prefix}{name}) / sizeof(({prefix}{name})[0])))"
    )
    return "\n".join(lines)


def regen_command(prog: str, args: argparse.Namespace) -> str:
    parts = ["python3", prog]
    if args.ucd:
        parts += ["--ucd", args.ucd]
    if args.prefix != DEFAULT_PREFIX:
        parts += ["--prefix", args.prefix]
    if args.type != DEFAULT_TYPE:
        parts += ["--type", args.type]
    if args.guard != args.prefix.upper() + "UNICODE_TABLES_H":
        parts += ["--guard", args.guard]
    if args.out:
        parts += ["--out", args.out]
    return " ".join(parts)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--prefix",
        default=DEFAULT_PREFIX,
        help=f"name prefix for the tables (default: {DEFAULT_PREFIX})",
    )
    ap.add_argument(
        "--type",
        default=DEFAULT_TYPE,
        help=f"range struct typedef name (default: {DEFAULT_TYPE})",
    )
    ap.add_argument(
        "--guard",
        default=None,
        help="include guard macro (default: <PREFIX>UNICODE_TABLES_H)",
    )
    ap.add_argument(
        "--ucd", default=None, help="UCD directory to read instead of downloading"
    )
    ap.add_argument("--out", default=None, help="write here instead of stdout")
    args = ap.parse_args(argv)
    if args.guard is None:
        args.guard = args.prefix.upper() + "UNICODE_TABLES_H"

    ucd = load_ucd(args.ucd) if args.ucd else download_ucd()

    eaw = parse_props(ucd["EastAsianWidth.txt"])
    gbp = parse_props(ucd["auxiliary/GraphemeBreakProperty.txt"])
    emo = parse_props(ucd["emoji/emoji-data.txt"])
    dcp = parse_props(ucd["DerivedCoreProperties.txt"])

    tables = [
        (
            "WIDE",
            "UAX #11 classes W + F — always two cells wide",
            coalesce(eaw.get("W", []) + eaw.get("F", [])),
        ),
        (
            "AMBIGUOUS",
            "UAX #11 class A — two cells only when the terminal says ambiguous_wide",
            coalesce(eaw.get("A", [])),
        ),
        (
            "ZERO",
            "zero width — GCB Extend ∪ Control ∪ ZWJ ∪ Default_Ignorable_Code_Point",
            coalesce(
                gbp.get("Extend", [])
                + gbp.get("Control", [])
                + gbp.get("ZWJ", [])
                + dcp.get("Default_Ignorable_Code_Point", [])
            ),
        ),
        (
            "EXTEND",
            "GCB Extend ∪ ZWJ — no grapheme break before these (GB9)",
            coalesce(gbp.get("Extend", []) + gbp.get("ZWJ", [])),
        ),
        (
            "SPACING_MARK",
            "GCB SpacingMark — no grapheme break before these (GB9a)",
            coalesce(gbp.get("SpacingMark", [])),
        ),
        (
            "PREPEND",
            "GCB Prepend — forces the next character into the cluster (GB9b)",
            coalesce(gbp.get("Prepend", [])),
        ),
        (
            "EXT_PICT",
            "Extended_Pictographic — GB11's ZWJ-suppression test",
            coalesce(emo.get("Extended_Pictographic", [])),
        ),
    ]

    versions = {rel: ucd_version(data) for rel, data in ucd.items()}
    unicode_version = versions["EastAsianWidth.txt"]
    gen_sha = hashlib.sha256(open(os.path.abspath(__file__), "rb").read()).hexdigest()[
        :16
    ]

    out = (
        sys.stdout
        if not args.out
        else open(args.out, "w", encoding="utf-8", newline="\n")
    )
    w = out.write

    w("/* Generated by gen_unicode_tables.py — DO NOT EDIT BY HAND.\n")
    w(" *\n")
    w(f" * UCD source: {UCD_BASE} (Unicode {unicode_version})\n")
    for relpath in UCD_FILES:
        w(
            f" *   {relpath:<38} {versions[relpath]:<8} sha256 {hashlib.sha256(ucd[relpath]).hexdigest()[:16]}\n"
        )
    w(
        f" *   {'gen_unicode_tables.py':<38} {'':<8} sha256 {gen_sha}  (this generator)\n"
    )
    w(" *\n")
    w(" * Regenerate:\n")
    w(f" *   {regen_command(sys.argv[0], args)}\n")
    w(" *\n")
    w(" * Tables (sorted, non-overlapping; binary-searched by range_lookup):\n")
    for name, desc, ranges in tables:
        w(f" *   {args.prefix}{name:<14} {desc}\n")
    w(" *\n")
    w(" * Data-only by design — no logic, nothing but <stdint.h> — so another\n")
    w(" * project can vendor this output with --prefix/--type and stay free of\n")
    w(" * any dependency on coffer. Regenerating in place is the only edit.\n")
    w(" */\n")
    w("\n")
    w(f"#ifndef {args.guard}\n")
    w(f"#define {args.guard}\n")
    w("\n")
    w("#include <stdint.h>\n")
    w("\n")
    w("typedef struct\n")
    w("{\n")
    w("    uint32_t lo, hi;\n")
    w(f"}} {args.type};\n")
    w("\n")
    w("/* Some translation units reach for only a few tables. */\n")
    w("#if defined(__GNUC__) || defined(__clang__)\n")
    w(f"#define {args.prefix}TABLE_UNUSED __attribute__((unused))\n")
    w("#else\n")
    w(f"#define {args.prefix}TABLE_UNUSED\n")
    w("#endif\n")
    for name, _desc, ranges in tables:
        w("\n")
        w(emit_table(name, ranges, args.prefix, args.type) + "\n")
    w("\n")
    w(f"#endif /* {args.guard} */\n")

    if args.out:
        out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
