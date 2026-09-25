#!/usr/bin/env python3
"""Assert the board's reported EFFECTIVE CAPS match the -D flags it was built with.

WHY THIS EXISTS
---------------
`check_config_guards.py` stops the known cause of a silently-ignored build flag
(a bare #define shadowing it). This checks the EFFECT, on the actual board, and
so catches causes nobody has thought of yet -- a flag misspelled in an env, a
stale binary that was never reflashed, an `extends` that does not inherit what
it looks like it inherits, a value clamped at runtime.

The firmware prints, once, at mount:

    [fs] EFFECTIVE CAPS: tier A max 10% = 406323 B, warn at 70%, snapshot 1000 ms

Nothing else a bench run produces reveals this. On 2026-09-24 the 18:40 run put
that information in front of nobody and burned 20 minutes measuring a cap it
did not have. A human reading a log is not a check -- this is.

EXIT CODES
    0  every mappable flag matches, or the env sets none
    1  MISMATCH -- the board is not running what was requested
    2  could not check (no caps line, unreadable ini, bad args)

USAGE
    python tools/assert_effective_caps.py --env esp32-can-x2-captest \\
        --serial analysis/bench-.../serial.log
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
INI = REPO / "firmware" / "platformio.ini"

CAPS_RE = re.compile(
    r"EFFECTIVE CAPS:\s*tier A max\s*(?P<tier_a_pct>\d+)%\s*=\s*"
    r"(?P<tier_a_bytes>\d+)\s*B,\s*warn at\s*(?P<warn_pct>\d+)%,\s*"
    r"snapshot\s*(?P<snapshot_ms>\d+)\s*ms")

# Which -D macros this line can actually speak for. A flag that is not here is
# reported as UNCHECKED rather than silently treated as fine -- the whole point
# is that silence is what went wrong before.
MACRO_TO_FIELD = {
    "FS_TIER_A_MAX_PCT": "tier_a_pct",
    "FS_WARN_USAGE_PCT": "warn_pct",
    "FS_SNAPSHOT_PERIOD_MS": "snapshot_ms",
}

DFLAG_RE = re.compile(r"-D([A-Za-z_][A-Za-z0-9_]*)(?:=(\S+))?")


def env_dflags(ini_path: Path, env: str) -> dict[str, str]:
    """-D flags for `env`, following `extends` so inherited flags count too."""
    text = ini_path.read_text(encoding="utf-8", errors="replace")
    sections: dict[str, list[str]] = {}
    current = None
    for line in text.splitlines():
        header = re.match(r"^\[([^\]]+)\]", line)
        if header:
            current = header.group(1)
            sections[current] = []
            continue
        if current:
            sections[current].append(line)

    def collect(name: str, depth: int = 0) -> dict[str, str]:
        if depth > 10 or name not in sections:
            return {}
        body = sections[name]
        flags: dict[str, str] = {}
        for line in body:
            m = re.match(r"^\s*extends\s*=\s*(\S+)", line)
            if m:
                flags.update(collect(m.group(1).strip(), depth + 1))
        for line in body:
            code = line.split(";", 1)[0]
            for macro, value in DFLAG_RE.findall(code):
                flags[macro] = value
        return flags

    for key in (f"env:{env}", env):
        if key in sections:
            return collect(key)
    raise SystemExit(f"FATAL: env '{env}' not found in {ini_path}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", required=True)
    ap.add_argument("--serial", required=True,
                    help="serial.log captured for this run")
    ap.add_argument("--ini", default=str(INI))
    ap.add_argument("--expect", action="append", default=[],
                    metavar="MACRO=VALUE",
                    help="Assert this value regardless of what the env sets. "
                         "Needed for PRODUCTION defaults: esp32-can-x2 passes "
                         "no -D for them, so without this the check has "
                         "nothing to compare and passes trivially. The "
                         "overnight soak must prove it is NOT carrying the "
                         "10%% test cap.")
    args = ap.parse_args()

    serial = Path(args.serial)
    if not serial.exists():
        print(f"CAPS ASSERT: cannot check -- {serial} does not exist")
        return 2

    text = serial.read_text(encoding="utf-8", errors="replace")
    matches = list(CAPS_RE.finditer(text))
    if not matches:
        print("CAPS ASSERT: cannot check -- no '[fs] EFFECTIVE CAPS' line in "
              f"{serial}.\n"
              "  The board is running firmware older than the line itself "
              "(cardiag c0c2645). Reflash before trusting this run.")
        return 2

    # The LAST one: if the board rebooted mid-run, the live state is the one
    # that matters.
    caps = matches[-1].groupdict()
    line = matches[-1].group(0)
    print(f"CAPS ASSERT: board reports -> {line}")

    flags = env_dflags(Path(args.ini), args.env)
    # --expect WINS over the env: it is an explicit statement of what this run
    # requires, and the case it exists for is an env that sets nothing at all.
    explicit: set[str] = set()
    for item in args.expect:
        if "=" not in item:
            print(f"CAPS ASSERT: bad --expect '{item}', want MACRO=VALUE")
            return 2
        macro, value = item.split("=", 1)
        if macro not in MACRO_TO_FIELD:
            print(f"CAPS ASSERT: --expect {macro} is not on the caps line; "
                  f"known: {', '.join(sorted(MACRO_TO_FIELD))}")
            return 2
        flags[macro] = value
        explicit.add(macro)
    if explicit:
        print(f"  asserting explicitly: "
              f"{', '.join(f'{m}={flags[m]}' for m in sorted(explicit))}")

    mismatches: list[str] = []
    checked = 0
    for macro, field in MACRO_TO_FIELD.items():
        if macro not in flags:
            continue
        want = flags[macro]
        if want == "":
            continue
        got = caps[field]
        checked += 1
        if str(want) != str(got):
            mismatches.append(
                f"  {macro}: requested {want}, board reports {got}")
        else:
            print(f"  OK  {macro} = {got}")

    unchecked = [m for m in flags
                 if m not in MACRO_TO_FIELD and m.startswith("FS_")]
    if unchecked:
        print(f"  UNCHECKED (not on the caps line): {', '.join(sorted(unchecked))}")

    if mismatches:
        print("\n*** CAPS ASSERT FAILED -- the board is NOT running what was "
              "requested ***")
        for m in mismatches:
            print(m)
        print("\nA run under a flag that did not take effect measures something "
              "other than what it claims. Aborting is correct: the numbers "
              "would be produced, and believed.")
        return 1

    if checked == 0:
        print("  (this env sets no flag the caps line can speak for)")
    print(f"CAPS ASSERT OK -- {checked} flag(s) verified on hardware")
    return 0


if __name__ == "__main__":
    sys.exit(main())
