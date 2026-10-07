#!/usr/bin/env python3
"""Per-function stack frame budget, from GCC's -fstack-usage output.

Why: the #29 panics were a ~5 KB handleSession() frame overflowing the
8 KB Arduino loop-task stack. Nothing at build time said so; the board
found it. This reads the .su files the build writes next to each object
(firmware/platformio.ini adds -fstack-usage), prints the 10 largest frames
in OUR code, and fails if any frame is over the budget and not allowlisted.

A .su line is:   path:line:col:signature<TAB>bytes<TAB>qualifier
qualifier is static | dynamic | dynamic,bounded. An unbounded `dynamic`
frame (VLA / alloca) has no known size, so it fails like an over-budget one.

The budget is per FRAME. It is not a call-chain depth: a 1.5 KB frame that
calls into LittleFS can still overflow. stackguard.h's loop_stack.free_min
in /api/v1/session is the runtime half of this check.

Allowlist (tools/stack_allow.txt): one entry per line,
    <file.cpp>:<function>  <max bytes>  # why this frame is justified
The entry is a CAP, not an exemption: the frame may grow up to it, no
further, so an allowlisted frame that doubles still fails.

Usage: python tools/stack_budget.py <dir-with-.su-files>... [--budget 2048]
       [--allow tools/stack_allow.txt] [--top 10]
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass

DEFAULT_BUDGET = 2048


@dataclass(frozen=True)
class Frame:
    file: str
    line: int
    func: str          # bare name, e.g. handleSession
    signature: str     # as GCC printed it
    bytes: int
    qualifier: str

    @property
    def key(self) -> str:
        return f"{self.file}:{self.func}"

    @property
    def unbounded(self) -> bool:
        return "dynamic" in self.qualifier and "bounded" not in self.qualifier


_NAME_RE = re.compile(r"([A-Za-z_~][A-Za-z0-9_.]*)\s*\(")
# GCC clones keep the original name plus a suffix: foo.constprop.0,
# foo.isra.0, foo.part.0, foo.cold. The key is the original name.
_CLONE_RE = re.compile(r"\.(constprop|isra|part|cold|lto_priv)(\.\d+)*")


def bare_name(signature: str) -> str:
    """'void ns::Cls::handleSession(WebServer&)' -> 'handleSession';
    'bool cdelfFind.constprop.0(const uint8_t*)' -> 'cdelfFind'.
    Lambdas and odd names fall back to the whole signature."""
    head = signature.split("(", 1)[0] + "("
    names = _NAME_RE.findall(head)
    if not names:
        return signature.strip()
    return _CLONE_RE.sub("", names[-1].split("::")[-1]).split(".")[0]


def parse_su_line(text: str) -> Frame | None:
    parts = text.rstrip("\n").split("\t")
    if len(parts) != 3:
        return None
    loc, size, qual = parts
    # path:line:col:signature -- the path itself may contain ':' on Windows,
    # so split from the left only after the last path separator.
    m = re.match(r"^(.*?):(\d+):(\d+):(.*)$", loc)
    if not m or not size.strip().isdigit():
        return None
    path, line, _col, sig = m.groups()
    return Frame(file=os.path.basename(path), line=int(line),
                 func=bare_name(sig), signature=sig.strip(),
                 bytes=int(size), qualifier=qual.strip())


def read_frames(dirs: list[str]) -> list[Frame]:
    frames: list[Frame] = []
    for d in dirs:
        for root, _subdirs, files in os.walk(d):
            for name in files:
                if not name.endswith(".su"):
                    continue
                with open(os.path.join(root, name), encoding="utf-8",
                          errors="replace") as fh:
                    for raw in fh:
                        f = parse_su_line(raw)
                        if f:
                            frames.append(f)
    return frames


def read_allowlist(path: str | None) -> dict[str, int]:
    allow: dict[str, int] = {}
    if not path or not os.path.exists(path):
        return allow
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            line = raw.split("#", 1)
            body = line[0].strip()
            if not body:
                continue
            fields = body.split()
            if len(fields) != 2 or ":" not in fields[0] or not fields[1].isdigit():
                raise SystemExit(f"{path}:{n}: expected '<file.cpp>:<function> <max bytes>  # why'")
            if len(line) < 2 or not line[1].strip():
                raise SystemExit(f"{path}:{n}: an allowlist entry needs a '# why' comment")
            allow[fields[0]] = int(fields[1])
    return allow


def violations(frames: list[Frame], budget: int,
               allow: dict[str, int]) -> list[tuple[Frame, str]]:
    out: list[tuple[Frame, str]] = []
    for f in frames:
        cap = allow.get(f.key)
        limit = cap if cap is not None else budget
        if f.unbounded and cap is None:
            out.append((f, "unbounded dynamic frame (VLA/alloca): size unknown"))
        elif f.bytes > limit:
            why = (f"{f.bytes} B > allowlisted cap {cap} B" if cap is not None
                   else f"{f.bytes} B > budget {budget} B")
            out.append((f, why))
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--budget", type=int, default=DEFAULT_BUDGET)
    ap.add_argument("--allow", default=None)
    ap.add_argument("--top", type=int, default=10)
    a = ap.parse_args(argv)

    frames = read_frames(a.dirs)
    if not frames:
        # No .su files means -fstack-usage is not in effect: the check would
        # otherwise pass by measuring nothing.
        print(f"::error::no .su files under {a.dirs}; is -fstack-usage in build_flags?")
        return 1
    allow = read_allowlist(a.allow)

    ranked = sorted(frames, key=lambda f: f.bytes, reverse=True)
    print(f"Largest stack frames ({len(frames)} functions, budget {a.budget} B):")
    for f in ranked[: a.top]:
        tag = f"  [allowlisted <= {allow[f.key]}]" if f.key in allow else ""
        print(f"  {f.bytes:6d} B  {f.qualifier:16s} {f.file}:{f.line}  {f.func}{tag}")

    stale = sorted(k for k in allow if k not in {f.key for f in frames})
    for k in stale:
        print(f"::warning::stack allowlist entry {k} matches no function (stale?)")

    bad = violations(frames, a.budget, allow)
    for f, why in bad:
        print(f"::error file=firmware/src/{f.file},line={f.line}::stack frame "
              f"{f.func}: {why}. Shrink it (static/heap buffer) or allowlist it "
              f"in tools/stack_allow.txt with a reason.")
    if bad:
        print(f"FAIL: {len(bad)} frame(s) over budget")
        return 1
    print("OK: every frame within budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
