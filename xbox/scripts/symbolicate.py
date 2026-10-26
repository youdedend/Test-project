#!/usr/bin/env python3
"""Resolve crash-report addresses (bloodborne.exe+0xNNNNNN) to symbols from an MSVC/lld map.

The Xbox UWP build links with /MAP, so every package has a matching bloodborne.map in which
"Rva+Base" is the image-base-relative address the fault report prints (image base 0x140000000,
/HIGHENTROPYVA:NO). A crash line such as

    Fault: read access to 0000020500987170 at bloodborne.exe+0x3003f0

is therefore resolved by asking this script for 0x3003f0.

Usage:
    python xbox/scripts/symbolicate.py --map xbox/.local/build/bloodborne.map \\
        --rvas xbox/scripts/crash-rvas.txt
    python xbox/scripts/symbolicate.py --map bloodborne.map 0x3003f0 0x26bbb7

Exit code is 0 when every address resolved to at least one symbol, 1 otherwise (so CI fails
loudly if /MAP ever stops producing a usable map).
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys

# " 0001:00000a30  ?func@@YAHXZ  0000000140001a30 f   main.obj"
MAP_LINE = re.compile(
    r"^\s*(?P<section>[0-9a-fA-F]{4}):(?P<offset>[0-9a-fA-F]{8})\s+"
    r"(?P<symbol>\S+)\s+(?P<address>[0-9a-fA-F]{16})"
)
PREFERRED_BASE = re.compile(r"Preferred load address is ([0-9a-fA-F]+)", re.IGNORECASE)


class Entry:
    __slots__ = ("rva", "section", "offset", "symbol")

    def __init__(self, rva: int, section: int, offset: int, symbol: str) -> None:
        self.rva = rva
        self.section = section
        self.offset = offset
        self.symbol = symbol

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"Entry({self.rva:#x}, {self.symbol})"


def parse_map(path: str) -> tuple[list[Entry], int]:
    """Returns (entries sorted by RVA, preferred image base)."""
    base = 0x140000000
    entries: list[Entry] = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if base == 0x140000000:
                match = PREFERRED_BASE.search(line)
                if match:
                    base = int(match.group(1), 16)
                    continue
            match = MAP_LINE.match(line)
            if not match:
                continue
            absolute = int(match.group("address"), 16)
            rva = absolute - base if absolute >= base else int(match.group("offset"), 16)
            entries.append(
                Entry(rva, int(match.group("section"), 16), int(match.group("offset"), 16),
                      match.group("symbol"))
            )
    # The map lists publics and static symbols in address order per section; sort by RVA and
    # drop duplicates that only repeat the same name (static/public aliases).
    entries.sort(key=lambda entry: (entry.rva, entry.symbol))
    unique: list[Entry] = []
    for entry in entries:
        if unique and unique[-1].rva == entry.rva and unique[-1].symbol == entry.symbol:
            continue
        unique.append(entry)
    return unique, base


def demangler():
    """undname.exe from the MSVC toolset, when the CI image has it on PATH."""
    for tool in ("undname", "undname.exe"):
        found = shutil.which(tool)
        if found:
            return found
    return None


def demangle(tool: str | None, symbol: str) -> str:
    if not tool or not symbol.startswith("?"):
        return symbol
    try:
        done = subprocess.run([tool, symbol], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return symbol
    for line in done.stdout.splitlines():
        line = line.strip()
        if line.startswith("Undecoration of") or not line:
            continue
        return line
    return symbol


def resolve(entries: list[Entry], target: int, tool: str | None) -> list[str]:
    if not entries:
        return []
    lower, upper = 0, len(entries)
    while lower < upper:  # first entry with rva > target
        middle = (lower + upper) // 2
        if entries[middle].rva <= target:
            lower = middle + 1
        else:
            upper = middle
    lines = []
    if lower == 0:
        return lines
    symbol = entries[lower - 1]
    lines.append(
        f"+0x{target:x} -> {demangle(tool, symbol.symbol)} "
        f"(+0x{target - symbol.rva:x} into the symbol, section {symbol.section:04x}, "
        f"starts at +0x{symbol.rva:x})"
    )
    if lower < len(entries):
        following = entries[lower]
        lines.append(
            f"          next symbol +0x{following.rva:x}: {demangle(tool, following.symbol)}"
        )
    # A map lists both a public symbol and its static/section siblings; print the neighbours so
    # an inlined frame (the common case for the GPU fault) can be attributed to its caller.
    for neighbour in range(max(0, lower - 3), min(len(entries), lower + 1)):
        entry = entries[neighbour]
        if entry is symbol:
            continue
        lines.append(f"          nearby  +0x{entry.rva:x}: {demangle(tool, entry.symbol)}")
    return lines


def read_rvas(path: str) -> list[int]:
    values = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            for token in line.replace(",", " ").split():
                try:
                    values.append(int(token, 16))
                except ValueError:
                    print(f"warning: ignoring unparsable RVA {token!r}", file=sys.stderr)
    return values


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--map", required=True, help="MSVC /MAP output (bloodborne.map)")
    parser.add_argument("--rvas", help="file with one or more hex RVAs per line (# comments allowed)")
    parser.add_argument("rva", nargs="*", help="hex RVAs, e.g. 0x3003f0")
    args = parser.parse_args(argv)

    try:
        entries, base = parse_map(args.map)
    except OSError as error:
        print(f"error: cannot read map {args.map}: {error}", file=sys.stderr)
        return 1
    if not entries:
        print(f"error: no symbols found in {args.map}; was the link done with /MAP?", file=sys.stderr)
        return 1

    targets: list[int] = []
    if args.rvas:
        targets.extend(read_rvas(args.rvas))
    for value in args.rva:
        try:
            targets.append(int(value, 0))
        except ValueError:
            print(f"warning: ignoring unparsable RVA {value!r}", file=sys.stderr)
    if not targets:
        print("error: no RVAs given (use --rvas FILE or positional 0xNNNNNN values)", file=sys.stderr)
        return 1

    tool = demangler()
    print(f"map      : {args.map}")
    print(f"base     : 0x{base:x}")
    print(f"symbols  : {len(entries)}")
    print("")
    unresolved = 0
    for target in targets:
        lines = resolve(entries, target, tool)
        if not lines:
            print(f"+0x{target:x} -> <no symbol at or below this address>")
            unresolved += 1
            continue
        print("\n".join(lines))
    print("")
    print(f"resolved {len(targets) - unresolved}/{len(targets)} addresses")
    return 0 if unresolved == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
