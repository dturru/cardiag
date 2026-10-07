"""tools/stack_budget.py: .su parsing, budget, allowlist caps."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import stack_budget as sb  # noqa: E402

SU = (
    "src/hubapi.cpp:57:13:void handleSession(WebServer&)\t5168\tstatic\n"
    "src/filestore.cpp:809:13:void drainChangeLog()\t2128\tstatic\n"
    "src/main.cpp:1010:6:void loop()\t96\tstatic\n"
    "src/x.cpp:5:6:void vla(int)\t32\tdynamic\n"
    "src/y.cpp:9:6:void bounded(int)\t64\tdynamic,bounded\n"
    "C:\\a\\b\\src\\z.cpp:3:6:int ns::Cls::method(int) const\t40\tstatic\n"
    "garbage line\n"
)


def write(tmp_path, text=SU, name="all.su"):
    p = tmp_path / name
    p.write_text(text)
    return str(tmp_path)


def test_parse_line_and_bare_name():
    f = sb.parse_su_line("src/hubapi.cpp:57:13:void handleSession(WebServer&)\t5168\tstatic")
    assert (f.file, f.line, f.func, f.bytes, f.qualifier) == (
        "hubapi.cpp", 57, "handleSession", 5168, "static")
    assert f.key == "hubapi.cpp:handleSession"
    assert sb.bare_name("int ns::Cls::method(int) const") == "method"
    # GCC clones: the key is the original function, not the clone suffix.
    assert sb.bare_name("bool cdelfFind.constprop.0(const uint8_t*, size_t)") == "cdelfFind"
    assert sb.bare_name("void foo.isra.0(int)") == "foo"
    assert sb.bare_name("void bar.part.0.cold(int)") == "bar"
    assert sb.parse_su_line("garbage line") is None


def test_over_budget_fails_and_top_list_prints(tmp_path, capsys):
    rc = sb.main([write(tmp_path), "--budget", "2048"])
    out = capsys.readouterr().out
    assert rc == 1
    assert "handleSession" in out and "5168 B" in out
    assert out.index("handleSession") < out.index("drainChangeLog")   # sorted
    errors = [l for l in out.splitlines() if l.startswith("::error")]
    assert any("vla: unbounded dynamic frame" in l for l in errors)
    assert not any("bounded:" in l for l in errors)    # dynamic,bounded is fine


def test_allowlist_is_a_cap_not_an_exemption(tmp_path, capsys):
    allow = tmp_path / "allow.txt"
    allow.write_text(
        "hubapi.cpp:handleSession 6000  # test\n"
        "filestore.cpp:drainChangeLog 2000  # cap below the frame\n"
        "x.cpp:vla 64  # known VLA\n")
    rc = sb.main([write(tmp_path), "--allow", str(allow)])
    out = capsys.readouterr().out
    assert rc == 1
    errors = [l for l in out.splitlines() if l.startswith("::error")]
    assert any("drainChangeLog: 2128 B > allowlisted cap 2000 B" in l for l in errors)
    assert not any("handleSession" in l for l in errors)   # within its cap
    allow.write_text(
        "hubapi.cpp:handleSession 6000  # test\n"
        "filestore.cpp:drainChangeLog 2200  # fits\n"
        "x.cpp:vla 64  # known VLA\n")
    assert sb.main([write(tmp_path), "--allow", str(allow)]) == 0


def test_no_su_files_is_a_failure(tmp_path, capsys):
    assert sb.main([str(tmp_path)]) == 1
    assert "no .su files" in capsys.readouterr().out


def test_allowlist_entry_needs_a_reason(tmp_path):
    allow = tmp_path / "allow.txt"
    allow.write_text("hubapi.cpp:handleSession 6000\n")
    try:
        sb.read_allowlist(str(allow))
    except SystemExit as e:
        assert "needs a '# why'" in str(e)
    else:
        raise AssertionError("missing reason accepted")


def test_committed_allowlist_parses():
    here = os.path.join(os.path.dirname(__file__), "..", "tools", "stack_allow.txt")
    allow = sb.read_allowlist(here)
    assert allow, "allowlist empty or missing"
    assert all(v > sb.DEFAULT_BUDGET for v in allow.values()), \
        "an entry at or under the budget is not needed"
