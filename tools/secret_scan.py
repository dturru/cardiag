"""Value-level secret scan: is any real secret about to be pushed?

🔑 THE POINT IS TO CHECK THE VALUES, NOT TO TRUST .gitignore.
Asking "is secrets.h ignored?" answers a different and much weaker question
than "does this exact string appear anywhere I am about to publish?". A secret
gets committed by being PASTED somewhere else -- a test fixture, a README
example, a debug printf, a config sample -- and every one of those is a file
.gitignore has no opinion about.

So: pull the values out of the secrets files, then search

  * every tracked and every untracked-but-not-ignored file (what a push sends)
  * every blob in every reachable commit (what a push has ALREADY sent)

Secrets are never printed. Output is SHA-256 prefixes only, so this is safe to
run with the output going anywhere.

    python tools/secret_scan.py --repo . --secrets firmware/include/secrets.h
    python tools/secret_scan.py --repo ../carhub --secrets ../carhub/secrets.env
    python tools/secret_scan.py --repo . --secrets a.h --secrets b.env --history
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys

# Values shorter than this are not secrets, they are words. Matching on "ok" or
# "1" would drown the report in noise and train you to ignore it.
MIN_SECRET_LEN = 8

# Placeholders that are SUPPOSED to be in the repo. Finding one is not a leak;
# it is the example file doing its job.
KNOWN_PLACEHOLDERS = {
    "cardiag-default", "change-me", "changeme", "your-token-here",
    "REPLACE_ME", "xxxxxxxx", "password", "secret",
}

# C #define "VALUE"  |  KEY=VALUE
#
# 🐛 `[^\S\n]` (horizontal whitespace), NOT `\s`. `\s` MATCHES NEWLINES, so on
#
#     MQTT_USERNAME=
#     MQTT_PASSWORD=hunter2
#
# the `\s*` after the first `=` ate the line break and the regex happily
# reported MQTT_USERNAME's value as "MQTT_PASSWORD=hunter2". The scanner then
# searched the repo for THAT literal -- so it was hunting for a string nobody
# has, while the real secret on the following line was never searched for at
# all. A scanner that reports CLEAN for the wrong reason is worse than no
# scanner, because it is believed.
PATTERNS = (
    re.compile(r'#[^\S\n]*define[^\S\n]+(\w+)[^\S\n]+"([^"]*)"'),
    re.compile(r'^[^\S\n]*(?:export[^\S\n]+)?([A-Z][A-Z0-9_]*)'
               r'[^\S\n]*=[^\S\n]*(.*)$', re.M),
)


def fp(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()[:12]


def extract(path: str) -> dict[str, str]:
    """Pull name -> value pairs out of a secrets file."""
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError as exc:
        print(f"  cannot read {path}: {exc}", file=sys.stderr)
        return {}
    out: dict[str, str] = {}
    skipped: list[str] = []
    for pat in PATTERNS:
        for m in pat.finditer(text):
            name, value = m.group(1), m.group(2).strip().strip('"').strip("'")
            if not value:
                skipped.append(f"{name} (no value set)")
            elif value in KNOWN_PLACEHOLDERS:
                skipped.append(f"{name} (placeholder)")
            elif len(value) < MIN_SECRET_LEN:
                skipped.append(f"{name} (shorter than {MIN_SECRET_LEN} chars)")
            else:
                out[name] = value
    # Printed, not silent. A key that is skipped must be VISIBLY accounted for,
    # otherwise "2 values found" in a file with 3 keys reads as success.
    for s in skipped:
        print(f"      -- skipped: {s}")
    return out


def git(repo: str, *args: str) -> str:
    return subprocess.run(["git", "-C", repo, *args],
                          capture_output=True, text=True).stdout


def pushable_files(repo: str) -> list[str]:
    """Exactly what a push would carry: tracked + untracked-not-ignored."""
    out = git(repo, "ls-files", "--cached", "--others", "--exclude-standard")
    return [p for p in out.splitlines() if p.strip()]


def scan_worktree(repo: str, secrets: dict[str, str]) -> list[tuple[str, str]]:
    hits = []
    files = pushable_files(repo)
    print(f"  {len(files)} file(s) would be pushed")
    for rel in files:
        full = os.path.join(repo, rel)
        try:
            if os.path.getsize(full) > 8 * 1024 * 1024:
                continue
            blob = open(full, "rb").read()
        except OSError:
            continue
        try:
            text = blob.decode("utf-8")
        except UnicodeDecodeError:
            text = blob.decode("latin-1", "ignore")
        for name, value in secrets.items():
            if value in text:
                hits.append((name, rel))
    return hits


def scan_history(repo: str, secrets: dict[str, str]) -> list[tuple[str, str]]:
    """Has a value EVER been committed? A push publishes history too."""
    hits = []
    revs = git(repo, "rev-list", "--all").split()
    if not revs:
        return hits
    print(f"  {len(revs)} commit(s) in history")
    for name, value in secrets.items():
        r = subprocess.run(
            ["git", "-C", repo, "grep", "-I", "--fixed-strings", "-l", value,
             *revs],
            capture_output=True, text=True)
        if r.stdout.strip():
            where = r.stdout.strip().splitlines()[:3]
            hits.append((name, "; ".join(where)))
    return hits


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=".")
    ap.add_argument("--secrets", action="append", required=True,
                    help="a secrets file to take values FROM; repeatable")
    ap.add_argument("--history", action="store_true",
                    help="also search every blob in every reachable commit")
    args = ap.parse_args(argv)

    repo = os.path.abspath(args.repo)
    print(f"repo: {repo}")

    secrets: dict[str, str] = {}
    for spec in args.secrets:
        if not os.path.exists(spec):
            print(f"  secrets file absent, skipped: {spec}")
            continue
        found = extract(spec)
        print(f"  {spec}: {len(found)} value(s)")
        for name, value in found.items():
            print(f"      {name:<20} len={len(value):<3} sha256={fp(value)}")
        secrets.update(found)

    if not secrets:
        print("\nNo secret values found to search for. That is either a clean "
              "example file or the wrong path -- check before trusting it.")
        return 2

    # A secrets file that is itself pushable is the one thing .gitignore IS
    # for, so it is still worth asserting.
    pushable = set(pushable_files(repo))
    for spec in args.secrets:
        rel = os.path.relpath(os.path.abspath(spec), repo).replace("\\", "/")
        if rel in pushable:
            print(f"\n*** {rel} IS ITSELF PUSHABLE -- it must be gitignored ***")

    print("\nworktree scan:")
    wt = scan_worktree(repo, secrets)
    print("history scan:" if args.history else "history scan: skipped")
    hist = scan_history(repo, secrets) if args.history else []

    print("\n" + "=" * 68)
    if not wt and not hist:
        print("CLEAN: no secret value appears in anything that would be "
              "pushed,")
        print("       nor in any reachable commit." if args.history
              else "       (history not searched -- pass --history).")
        return 0
    print("*** LEAK ***")
    for name, where in wt:
        print(f"  worktree  {name} (sha256={fp(secrets[name])}) -> {where}")
    for name, where in hist:
        print(f"  history   {name} (sha256={fp(secrets[name])}) -> {where}")
    print("\nDo NOT push. Rotate the value, then remove it from history.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
