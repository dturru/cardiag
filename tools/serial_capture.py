"""Capture timestamped serial output for N seconds, non-interactively.

`pio device monitor` is interactive and cannot be scripted, which makes every
bring-up observation a thing a human has to watch and retype. This writes the
same stream to a file with a monotonic timestamp per line, so a bring-up step
produces EVIDENCE rather than a recollection.

    python tools/serial_capture.py --port COM3 --seconds 20 --out boot.log
    python tools/serial_capture.py --port COM3 --seconds 30 --send "s" --delay 2

`--send` writes a key to the device's serial console after `--delay` seconds,
which is how the firmware's single-key commands (mode changes, status) are
driven without a human at the keyboard.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

# esptool implements the S3's actual reset sequence. A hand-rolled DTR/RTS
# pulse did NOT work -- see hard_reset().
_PIO_PY = os.path.expanduser(r"~\.platformio\penv\Scripts\python.exe")
_ESPTOOL = os.path.expanduser(
    r"~\.platformio\packages\tool-esptoolpy\esptool.py")


def hard_reset(port: str) -> bool:
    """Reset the board via esptool, BEFORE the capture port is opened.

    🐛 WHY NOT A DTR/RTS PULSE HERE. This used to be exactly that -- pin DTR
    low, pulse RTS high then low -- and measured on COM3 2026-09-24 it DID
    NOTHING: board uptime kept climbing through 122 s. A flag that silently
    does nothing is worse than no flag, because the caller believes it worked
    and then reads a stale banner as though it were fresh. esptool already
    implements the real sequence for the S3's native USB-Serial-JTAG, so call
    that rather than reimplementing it badly.

    Returns True on success; the caller decides whether failure is fatal.
    """
    if not (os.path.exists(_PIO_PY) and os.path.exists(_ESPTOOL)):
        print(f"--reset: esptool not found at {_ESPTOOL}", file=sys.stderr)
        return False
    try:
        r = subprocess.run(
            [_PIO_PY, _ESPTOOL, "--chip", "esp32s3", "--port", port,
             "--after", "hard_reset", "read_mac"],
            capture_output=True, text=True, timeout=60,
            env={**os.environ, "PYTHONIOENCODING": "utf-8"})
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"--reset: esptool failed: {exc}", file=sys.stderr)
        return False
    if r.returncode != 0:
        print(f"--reset: esptool exited {r.returncode}\n{r.stderr[-400:]}",
              file=sys.stderr)
        return False
    return True


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--out", help="write here as well as to stdout")
    ap.add_argument("--send", action="append", default=[],
                    metavar="KEYS", help="send these bytes; repeatable")
    ap.add_argument("--delay", type=float, default=2.0,
                    help="seconds to wait before the first --send")
    ap.add_argument("--gap", type=float, default=1.0,
                    help="seconds between successive --send values")
    ap.add_argument("--reset", action="store_true",
                    help="hard-reset the board via esptool BEFORE capturing, "
                         "so the boot banner is in the log. Exits non-zero if "
                         "the reset fails rather than capturing a stale boot.")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    try:
        import serial
    except ImportError:
        print("pyserial is required: python -m pip install pyserial",
              file=sys.stderr)
        return 2

    # 🐛 OPENING OR CLOSING THIS PORT RESETS THE BOARD unless DTR/RTS are
    # pinned. The ESP32-S3's native USB-Serial-JTAG maps them to EN/BOOT, and
    # pyserial asserts DTR on open and drops both on close. That cost a boot_id
    # (12 -> 13) mid-bring-up and, worse, orphaned a SELFTEST .part whose
    # provenance then could not be recovered -- which is how the
    # `"synthetic":false` bug was found.
    #
    # `dsrdtr=False` + explicitly clearing both before anything else is what
    # makes an observation non-destructive. A capture tool that reboots the
    # thing it is observing is not a capture tool.
    # 🔑 RESET BEFORE OPENING, not after. esptool needs the port to itself, and
    # the point of --reset is to catch the boot banner, which setup() prints
    # ~2 s in -- so the capture must already be listening when it arrives.
    if args.reset and not hard_reset(args.port):
        print("--reset FAILED -- refusing to capture and call it a fresh boot",
              file=sys.stderr)
        return 3

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.1
    ser.dtr = False
    ser.rts = False
    ser.dsrdtr = False
    ser.open()

    fh = open(args.out, "w", encoding="utf-8", newline="") if args.out else None
    t0 = time.monotonic()
    pending = list(args.send)
    next_send = t0 + args.delay
    buf = b""
    try:
        while time.monotonic() - t0 < args.seconds:
            now = time.monotonic()
            if pending and now >= next_send:
                keys = pending.pop(0)
                ser.write(keys.encode("utf-8"))
                ser.flush()
                line = f"[{now - t0:8.3f}] >>> SENT {keys!r}"
                if not args.quiet:
                    print(line, flush=True)
                if fh:
                    fh.write(line + "\n")
                    fh.flush()
                next_send = now + args.gap

            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").rstrip("\r")
                line = f"[{time.monotonic() - t0:8.3f}] {text}"
                if not args.quiet:
                    print(line, flush=True)
                if fh:
                    # ⭐ FLUSH EVERY LINE. An unattended run can be killed at any
                    # moment, and a buffered tail is the part you most want --
                    # the lines just before it died. Also lets another process
                    # tail this file live, which is how the runner discovers the
                    # board's IP while the capture is still going.
                    fh.write(line + "\n")
                    fh.flush()
    finally:
        if buf:
            tail = f"[{time.monotonic() - t0:8.3f}] {buf.decode('utf-8', 'replace')}"
            if not args.quiet:
                print(tail, flush=True)
            if fh:
                fh.write(tail + "\n")
        if fh:
            fh.close()
        # Leave the lines where they were, so closing does not pulse EN/BOOT
        # and reboot the board we just finished observing.
        try:
            ser.dtr = False
            ser.rts = False
        except OSError:
            pass
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
