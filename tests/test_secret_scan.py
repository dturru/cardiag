"""secret_scan.py: the allowlist skips address keys and nothing else."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import secret_scan as sc  # noqa: E402

URL = "https://ntfy.sh"
TOKEN = "tk_abcdefghijklmnopqrstuvwxyz12"


def _repo(tmp_path: Path, tracked: str) -> Path:
    r = tmp_path / "repo"
    r.mkdir()
    subprocess.run(["git", "init", "-q", str(r)], check=True)
    (r / "bootstrap.sh").write_text(tracked, encoding="utf-8")
    return r


def _secrets(tmp_path: Path) -> Path:
    p = tmp_path / "secrets.env"
    p.write_text(f"NTFY_URL={URL}\nNTFY_TOKEN={TOKEN}\n", encoding="utf-8")
    return p


def test_committed_allowlist_lists_ntfy_url_and_no_credentials():
    allow = sc.load_allowlist()
    assert "NTFY_URL" in allow
    assert not any(sc.NEVER_ALLOW.search(n) for n in allow)


def test_listed_key_is_skipped(tmp_path, capsys):
    repo = _repo(tmp_path, f'curl "{URL}/v1/health"\n')
    rc = sc.main(["--repo", str(repo), "--secrets", str(_secrets(tmp_path))])
    out = capsys.readouterr().out
    assert rc == 0, out
    assert "NTFY_URL (allowlisted: not a secret)" in out


def test_unlisted_key_still_fires(tmp_path, capsys):
    repo = _repo(tmp_path, f'curl "{URL}" -H "Bearer {TOKEN}"\n')
    rc = sc.main(["--repo", str(repo), "--secrets", str(_secrets(tmp_path))])
    out = capsys.readouterr().out
    assert rc == 1
    assert "LEAK" in out and "NTFY_TOKEN" in out
    assert "worktree  NTFY_URL" not in out


def test_an_empty_allowlist_scans_the_url_too(tmp_path):
    found = sc.extract(str(_secrets(tmp_path)), allow=set())
    assert found == {"NTFY_URL": URL, "NTFY_TOKEN": TOKEN}


def test_credential_like_names_cannot_be_allowlisted(tmp_path):
    p = tmp_path / "allow.txt"
    p.write_text("NTFY_URL\nNTFY_TOKEN\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="NTFY_TOKEN"):
        sc.load_allowlist(str(p))
