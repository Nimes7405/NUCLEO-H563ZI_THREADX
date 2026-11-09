#!/usr/bin/env python3
"""Serial assertions for the Tx-Reliability-Monitor firmware."""

import argparse
import os
import re
import select
import sys
import termios
import time


class Port:
    def __init__(self, path, baud):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        cc = attrs[6]
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        speed = getattr(termios, f"B{baud}")
        termios.tcsetattr(
            self.fd,
            termios.TCSANOW,
            [0, 0, termios.CS8 | termios.CREAD | termios.CLOCAL, 0, speed, speed, cc],
        )
        self.buf = b""

    def clear(self):
        self.buf = b""

    def drain_quiet(self, quiet=0.3, max_total=2.0):
        """Read until the line has been quiet for `quiet` seconds, then clear."""
        end = time.time() + max_total
        last = time.time()
        while time.time() < end:
            before = len(self.buf)
            self.pump(0.05)
            if len(self.buf) != before:
                last = time.time()
            elif time.time() - last >= quiet:
                break
        self.clear()

    def pump(self, timeout):
        r, _, _ = select.select([self.fd], [], [], max(0.0, timeout))
        if not r:
            return
        try:
            data = os.read(self.fd, 4096)
        except BlockingIOError:
            return
        if data:
            self.buf += data

    def read_for(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(min(0.1, end - time.time()))

    def wait_for(self, needle, timeout):
        needle = needle.encode()
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.buf:
                return True
            self.pump(min(0.1, end - time.time()))
        return needle in self.buf

    def send(self, line):
        os.write(self.fd, (line + "\n").encode())
        time.sleep(0.05)

    def text(self):
        return self.buf.decode("utf-8", "replace")

    def close(self):
        os.close(self.fd)


def check(port, name, condition, missing):
    if condition:
        print(f"PASS {name}")
        return True
    print(f"FAIL {name}: missing {missing!r}")
    return False


CTRL_RE = re.compile(
    r"CTRL mode=(\w+) on=(\d) sp=(-?\d+) fb=(-?\d+) adc=(\d+) out=(-?\d+) "
    r"err=(-?\d+) jit_us=([0-9.]+)/([0-9.]+) ovr=(\d+) cnt=(\d+)"
)


def parse_ctrl(line):
    match = CTRL_RE.search(line)
    if not match:
        return None
    return {
        "mode": match.group(1),
        "on": int(match.group(2)),
        "sp": int(match.group(3)),
        "fb": int(match.group(4)),
        "adc": int(match.group(5)),
        "out": int(match.group(6)),
        "err": int(match.group(7)),
        "jit_min": float(match.group(8)),
        "jit_max": float(match.group(9)),
        "ovr": int(match.group(10)),
        "cnt": int(match.group(11)),
    }


def mode_control(port):
    port.drain_quiet()
    port.send("fault clear")
    port.read_for(0.15)
    port.send("ctrl on")
    port.send("plant sim")
    port.send("set 1000")
    port.read_for(0.8)
    port.clear()
    port.send("set 3000")
    port.read_for(2.0)

    lines = [parse_ctrl(line) for line in port.text().splitlines()]
    lines = [item for item in lines if item is not None and item["sp"] == 3000]
    ok = check(port, "ctrl-lines", len(lines) >= 2, "CTRL sp=3000")

    if lines:
        last = lines[-1]
        ok &= check(port, "mode-sim", last["mode"] == "sim", f"mode={last['mode']}")
        ok &= check(port, "ctrl-on", last["on"] == 1, f"on={last['on']}")
        ok &= check(port, "setpoint", last["sp"] == 3000, f"sp={last['sp']}")
        ok &= check(port, "feedback", abs(last["fb"] - 3000) <= 30, f"fb={last['fb']}")
        ok &= check(port, "overruns", last["ovr"] == 0, f"ovr={last['ovr']}")
        spread = last["jit_max"] - last["jit_min"]
        ok &= check(port, "jitter-us", spread <= 5.0, f"spread={spread:.2f}us")
        print(f"INFO ctrl sp={last['sp']} fb={last['fb']} out={last['out']} "
              f"jit={last['jit_min']:.2f}/{last['jit_max']:.2f}us ovr={last['ovr']} cnt={last['cnt']}")

    port.clear()
    port.send("cal show")
    ok &= check(port, "cal-show", port.wait_for("CAL offset=0.00 scale=1.000", 2.0),
                "CAL offset=0.00 scale=1.000")
    port.clear()
    port.send("cal set 100 1.0")
    ok &= check(port, "cal-set", port.wait_for("CAL offset=100.00 scale=1.000", 2.0),
                "CAL offset=100.00 scale=1.000")
    port.clear()
    port.send("cal reset")
    ok &= check(port, "cal-reset", port.wait_for("CAL offset=0.00 scale=1.000", 2.0),
                "CAL offset=0.00 scale=1.000")
    return ok


def mode_basic(port):
    port.drain_quiet()
    port.send("reboot")
    port.wait_for("REBOOT requested", 2.0)
    port.clear()
    ok = port.wait_for("BOOT reset=", 10.0)
    ok &= check(port, "ready", port.wait_for("READY", 5.0), "READY")
    port.send("fault clear")
    port.read_for(0.2)
    port.send("status")
    port.send("stack")
    port.send("fault")
    port.send("wdg")
    port.send("log selfcheck-basic")
    port.read_for(1.0)
    text = port.text()
    ok &= check(port, "boot-banner", "BOOT reset=" in text, "BOOT reset=")
    ok &= check(port, "status", "STATUS ver=1.0.0" in text, "STATUS ver=1.0.0")
    ok &= check(port, "stack", "STACK monitor=" in text, "STACK monitor=")
    ok &= check(port, "fault", "FAULT " in text, "FAULT ")
    ok &= check(port, "wdg", "WDG timeout_ms=4000" in text, "WDG timeout_ms=4000")
    ok &= check(port, "log-echo", "LOG selfcheck-basic" in text, "LOG selfcheck-basic")
    ok &= check(port, "no-stall", "WATCHDOG stall=" not in text, "WATCHDOG stall=")
    ok &= check(port, "no-canary", "stack-canary-overwritten" not in text, "stack-canary-overwritten")
    return ok


def mode_watchdog(port):
    port.drain_quiet()
    port.send("stall 3")
    ok = port.wait_for("WATCHDOG stall=control", 8.0)
    ok &= check(port, "control-stall-detected", ok, "WATCHDOG stall=control")
    port.clear()
    ok &= check(port, "iwdg-reset", port.wait_for("BOOT reset=iwdg", 10.0), "BOOT reset=iwdg")
    ok &= check(port, "fault-none", port.wait_for("fault=none", 2.0), "fault=none")
    ok &= check(port, "ready-after-reset", port.wait_for("READY", 5.0), "READY")
    return ok


def mode_fault(port):
    port.drain_quiet()
    port.send("fault clear")
    port.read_for(0.15)
    port.clear()
    port.send("crash")
    port.wait_for("CRASH triggering", 2.0)
    port.clear()
    ok = port.wait_for("BOOT reset=software fault=yes", 10.0)
    ok &= check(port, "fault-boot", ok, "BOOT reset=software fault=yes")
    port.clear()
    ok &= check(port, "fault-record", port.wait_for("FAULT seq=", 3.0), "FAULT seq=")
    ok &= check(port, "busfault-cfsr", port.wait_for("cfsr=0x00008200", 2.0), "cfsr=0x00008200")
    ok &= check(port, "busfault-bfar", port.wait_for("bfar=0xfffffff0", 2.0), "bfar=0xfffffff0")
    port.clear()
    port.send("fault")
    ok &= check(port, "fault-command", port.wait_for("FAULT seq=", 3.0), "FAULT seq=")
    port.send("fault clear")
    return ok


def mode_mpu(port):
    port.drain_quiet()
    port.send("fault clear")
    port.read_for(0.15)
    port.clear()
    port.send("guard")
    ok = port.wait_for("GUARD triggering write to 0x", 2.0)
    match = re.search(r"GUARD triggering write to (0x[0-9a-fA-F]+)", port.text())
    address = match.group(1) if match else None
    ok &= check(port, "guard-trigger", ok, "GUARD triggering write to 0x")
    ok &= check(port, "fault-boot", port.wait_for("BOOT reset=software fault=yes", 5.0),
                "BOOT reset=software fault=yes")
    ok &= check(port, "fault-cfsr", port.wait_for("cfsr=0x00000082", 2.0), "cfsr=0x00000082")
    ok &= check(port, "guard-name", port.wait_for("FAULT guard=prod_a", 2.0), "FAULT guard=prod_a")
    ok &= check(port, "guard-mmfar", address is not None and port.wait_for(f"mmfar={address}", 2.0),
                f"mmfar={address}")
    port.send("fault clear")
    return ok


def mode_wwdg(port):
    port.drain_quiet()
    port.send("stall 4")
    ok = port.wait_for("BOOT reset=wwdg", 3.0)
    ok &= check(port, "wwdg-reset", ok, "BOOT reset=wwdg")
    ok &= check(port, "fault-none", port.wait_for("fault=none", 2.0), "fault=none")
    ok &= check(port, "ready", port.wait_for("READY", 5.0), "READY")
    return ok


def mode_release(port):
    port.read_for(0.5)
    boot_text = port.text()
    port.clear()
    port.send("help")
    port.send("status")
    port.send("stack")
    port.send("fault")
    port.send("wdg")
    port.send("log release-check")
    port.send("cal show")
    port.read_for(0.8)
    text = boot_text + port.text()
    ok = check(port, "boot-profile", "profile=release" in text, "profile=release")
    ok &= check(port, "status-profile", "STATUS ver=1.0.0" in text and "profile=release" in text,
                "STATUS ... profile=release")
    ok &= check(port, "stack", "STACK monitor=" in text, "STACK monitor=")
    ok &= check(port, "fault", "FAULT " in text, "FAULT ")
    ok &= check(port, "wdg", "WDG timeout_ms=4000" in text, "WDG timeout_ms=4000")
    ok &= check(port, "log-echo", "LOG release-check" in text, "LOG release-check")
    ok &= check(port, "cal", "CAL offset=" in text, "CAL offset=")
    help_line = ""
    for line in text.splitlines():
        if line.startswith("HELP "):
            help_line = line
    ok &= check(port, "no-guard", "guard" not in help_line, "guard absent from HELP")
    ok &= check(port, "no-stall", "stall" not in help_line, "stall absent from HELP")
    ok &= check(port, "no-crash", "crash" not in help_line, "crash absent from HELP")
    ok &= check(port, "no-reboot", "reboot" not in help_line, "reboot absent from HELP")
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mode", choices=["basic", "control", "mpu", "wwdg", "watchdog", "fault", "release"], default="basic")
    args = parser.parse_args()

    port = Port(args.port, args.baud)
    try:
        port.read_for(0.3)
        port.clear()
        if args.mode == "basic":
            ok = mode_basic(port)
        elif args.mode == "control":
            ok = mode_control(port)
        elif args.mode == "mpu":
            ok = mode_mpu(port)
        elif args.mode == "wwdg":
            ok = mode_wwdg(port)
        elif args.mode == "watchdog":
            ok = mode_watchdog(port)
        elif args.mode == "release":
            ok = mode_release(port)
        else:
            ok = mode_fault(port)
    finally:
        port.close()

    if not ok:
        print("\n--- captured output ---")
        print(port.text()[-3000:])
        return 1
    print(f"RESULT {args.mode}: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
