"""Every transmit on the vehicle bus goes through ONE gate (firmware/include/cantx.h).

The gate (canTxGate in powerpolicy.h) refuses all TX while the ignition is
off under POWER_POLICY=1. It only works if nothing goes around it, so the
only call to twai_transmit() allowed in firmware/src is inside canTransmit()
in power.cpp.
"""

import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "firmware" / "src"


def _code(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def test_twai_transmit_only_inside_the_gate():
    hits = {}
    for f in sorted(SRC.glob("*.cpp")):
        n = len(re.findall(r"\btwai_transmit\s*\(", _code(f.read_text(encoding="utf-8"))))
        if n:
            hits[f.name] = n
    assert hits == {"power.cpp": 1}, hits


def test_gate_checks_before_transmitting():
    body = _code((SRC / "power.cpp").read_text(encoding="utf-8"))
    fn = body[body.index("esp_err_t canTransmit("):]
    assert fn.index("canTxAllowed()") < fn.index("twai_transmit(")


def test_transmitting_twai_modes_are_gated():
    main = _code((SRC / "main.cpp").read_text(encoding="utf-8"))
    fn = main[main.index("static twai_mode_t twaiModeFor("):]
    fn = fn[:fn.index("\n}\n")]
    assert fn.index("canTxAllowed()") < fn.index("TWAI_MODE_NO_ACK")
