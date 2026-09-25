#!/usr/bin/env python3
"""Fail if a macro any PlatformIO env overrides with -D is not #ifndef-guarded.

WHY THIS EXISTS
---------------
A `-D` flag that a header later redefines is SILENTLY IGNORED. The compiler
does not error, the build succeeds, the test runs, and it measures something
other than what it says it does. It has now cost two separate bench campaigns:

  FS_SNAPSHOT_PERIOD_MS   `-DFS_SNAPSHOT_PERIOD_MS=20` in the fstest env did
                          nothing; a build that claimed to fill flash 50x
                          faster filled it at the normal rate.
  FS_TIER_A_MAX_PCT       `-DFS_TIER_A_MAX_PCT=10` in the captest env did
                          nothing; the 2026-09-24 18:40 run believed its Tier A
                          cap was 406 kB when it was 1,625 kB, and claim 2 went
                          NOT EXERCISED for the fourth time.

Both were the same one-line mistake in the same header, one `#define` apart.
The comment warning about the first sat six lines below the second. A comment
is not a control; this script is.

THE RULE
--------
The source of truth is platformio.ini. If ANY env sets a macro with `-D`, then
every definition of that macro in the firmware's own headers and sources must
be wrapped in `#ifndef` / `#endif`. Nothing else is checked -- a macro no env
overrides is free to be a bare #define.

That rule is self-maintaining: add a `-D` flag for a new macro and this script
starts requiring the guard on it, without anyone remembering to update a list.

USAGE
    python tools/check_config_guards.py            # exit 1 on any violation
    python tools/check_config_guards.py --list     # show what is checked
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIRMWARE = REPO / "firmware"
INI = FIRMWARE / "platformio.ini"

# Where a #define could shadow a -D flag. .pio is build output, never source.
SOURCE_GLOBS = ("include/**/*.h", "include/**/*.hpp",
                "src/**/*.h", "src/**/*.hpp", "src/**/*.cpp", "src/**/*.c")

# `-DNAME` or `-DNAME=value`. A bare -DNAME (no value) is still an override.
DFLAG_RE = re.compile(r"-D([A-Za-z_][A-Za-z0-9_]*)")


def overridden_macros(ini_path: Path) -> set[str]:
    """Every macro name any env passes with -D, in any build_flags block."""
    if not ini_path.exists():
        raise SystemExit(f"FATAL: {ini_path} not found")
    text = ini_path.read_text(encoding="utf-8", errors="replace")
    names: set[str] = set()
    for line in text.splitlines():
        # Strip inline comments so a -D inside prose is not counted.
        code = line.split(";", 1)[0]
        names.update(DFLAG_RE.findall(code))
    return names


def find_unguarded(macro: str, path: Path) -> list[tuple[int, str]]:
    """Definitions of `macro` in `path` that are NOT inside an #ifndef guard.

    Deliberately simple and local: a definition counts as guarded when an
    `#ifndef MACRO` (or `#if !defined(MACRO)`) appears above it with no
    intervening `#endif`. That is the shape the codebase uses, and a checker
    that tried to model the full preprocessor would be harder to trust than
    the thing it checks.
    """
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    define_re = re.compile(rf"^\s*#\s*define\s+{re.escape(macro)}\b")
    ifndef_re = re.compile(
        rf"^\s*#\s*(ifndef\s+{re.escape(macro)}\b"
        rf"|if\s+!\s*defined\s*\(\s*{re.escape(macro)}\s*\))")
    endif_re = re.compile(r"^\s*#\s*endif\b")

    bad: list[tuple[int, str]] = []
    guard_open = False
    for n, line in enumerate(lines, start=1):
        if ifndef_re.match(line):
            guard_open = True
            continue
        if endif_re.match(line):
            guard_open = False
            continue
        if define_re.match(line) and not guard_open:
            bad.append((n, line.strip()))
    return bad


def sources() -> list[Path]:
    found: list[Path] = []
    for pattern in SOURCE_GLOBS:
        found.extend(p for p in FIRMWARE.glob(pattern) if ".pio" not in p.parts)
    return sorted(set(found))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true",
                    help="print the macros checked and where they are defined")
    args = ap.parse_args()

    macros = overridden_macros(INI)
    files = sources()
    if not files:
        print(f"FATAL: no firmware sources found under {FIRMWARE}")
        return 2

    violations: list[tuple[str, Path, int, str]] = []
    seen: dict[str, list[str]] = {}

    for macro in sorted(macros):
        for path in files:
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if macro not in text:
                continue
            for lineno, src in find_unguarded(macro, path):
                violations.append((macro, path, lineno, src))
            if re.search(rf"^\s*#\s*define\s+{re.escape(macro)}\b",
                         text, re.MULTILINE):
                seen.setdefault(macro, []).append(
                    str(path.relative_to(REPO)).replace("\\", "/"))

    if args.list:
        print(f"macros overridden by a -D flag in {INI.name}: {len(macros)}")
        for macro in sorted(macros):
            where = ", ".join(seen.get(macro, [])) or "(not defined in-tree)"
            print(f"  {macro:<28} {where}")
        print()

    if violations:
        print("*** CONFIG GUARD CHECK FAILED ***\n")
        print("These macros are overridden by a -D flag in platformio.ini but")
        print("are defined WITHOUT an #ifndef guard. The header wins, so the")
        print("build flag does nothing and any run using it measures something")
        print("other than what it claims.\n")
        for macro, path, lineno, src in violations:
            rel = str(path.relative_to(REPO)).replace("\\", "/")
            print(f"  {rel}:{lineno}")
            print(f"      {src}")
            print(f"      -> wrap in  #ifndef {macro} / #endif\n")
        print(f"{len(violations)} violation(s).")
        return 1

    print(f"config guard check OK -- {len(macros)} overridable macro(s), "
          f"all guarded or not defined in-tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
