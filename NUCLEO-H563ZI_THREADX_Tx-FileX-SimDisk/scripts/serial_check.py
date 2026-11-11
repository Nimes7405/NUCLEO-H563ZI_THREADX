#!/usr/bin/env python3
"""Serial assertions for the Tx-Reliability-Monitor firmware."""

import argparse
import os
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


def mode_basic(port):
    port.drain_quiet()
    port.send("reboot")
    port.wait_for("REBOOT requested", 2.0)
    port.clear()
    ok = port.wait_for("BOOT reset=", 10.0)
    ok &= check(port, "ready", port.wait_for("READY", 5.0), "READY")
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
    port.send("stall 1")
    ok = port.wait_for("WATCHDOG stall=prod_a", 8.0)
    ok &= check(port, "stall-detected", ok, "WATCHDOG stall=prod_a")
    port.clear()
    ok &= check(port, "iwdg-reset", port.wait_for("BOOT reset=iwdg", 10.0), "BOOT reset=iwdg")
    ok &= check(port, "fault-none", port.wait_for("fault=none", 2.0), "fault=none")
    ok &= check(port, "ready-after-reset", port.wait_for("READY", 5.0), "READY")
    return ok


def mode_fault(port):
    port.drain_quiet()
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
    return ok


def mode_fs(port):
    port.drain_quiet()

    port.clear()
    port.send("fs info")
    ok = check(port, "fs-mounted", port.wait_for("FSD mounted=1", 3.0), "FSD mounted=1")
    ok &= check(port, "fs-geometry", port.wait_for("sectors=120", 2.0), "sectors=120")

    port.clear()
    port.send("fs test")
    ok &= check(port, "fs-selftest", port.wait_for("FS SELFTEST PASS", 8.0), "FS SELFTEST PASS")

    port.clear()
    port.send("fs write DEMO.TXT hello-simdisk")
    ok &= check(port, "fs-write",
                port.wait_for("FS WROTE name=DEMO.TXT bytes=13 rc=0", 5.0),
                "FS WROTE name=DEMO.TXT bytes=13 rc=0")

    port.clear()
    port.send("fs read DEMO.TXT")
    ok &= check(port, "fs-read",
                port.wait_for("FS DATA name=DEMO.TXT bytes=13 text=hello-simdisk", 5.0),
                "FS DATA ... text=hello-simdisk")

    port.clear()
    port.send("fs append DEMO.TXT +more")
    ok &= check(port, "fs-append", port.wait_for("FS WROTE name=DEMO.TXT bytes=5 rc=0", 5.0),
                "FS WROTE name=DEMO.TXT bytes=5 rc=0")
    port.clear()
    port.send("fs read DEMO.TXT")
    ok &= check(port, "fs-appended",
                port.wait_for("text=hello-simdisk+more", 5.0), "text=hello-simdisk+more")

    port.clear()
    port.send("fs ls")
    ok &= check(port, "fs-ls", port.wait_for("FS f=DEMO.TXT", 5.0), "FS f=DEMO.TXT")

    port.clear()
    port.send("fs fill 16")
    ok &= check(port, "fs-fill", port.wait_for("FS FILL PASS bytes=16384", 15.0),
                "FS FILL PASS bytes=16384")

    port.clear()
    port.send("fs stress 20")
    ok &= check(port, "fs-stress", port.wait_for("FS STRESS PASS runs=20", 25.0),
                "FS STRESS PASS runs=20")

    port.clear()
    port.send("fs info")
    ok &= check(port, "fs-nor-semantics", port.wait_for("violations=0", 3.0), "violations=0")
    ok &= check(port, "fs-erase-happened", port.wait_for("FSD nor blocks=8", 2.0), "FSD nor blocks=8")

    port.clear()
    port.send("fs umount")
    ok &= check(port, "fs-umount", port.wait_for("FS umount=ok rc=0", 5.0), "FS umount=ok rc=0")
    port.clear()
    port.send("fs mount")
    ok &= check(port, "fs-remount", port.wait_for("FS mount=ok rc=0", 8.0), "FS mount=ok rc=0")
    port.clear()
    port.send("fs ls")
    ok &= check(port, "fs-remount-kept",
                port.wait_for("FS f=FILL.BIN", 5.0), "FS f=FILL.BIN")

    port.clear()
    port.send("fs rm DEMO.TXT")
    ok &= check(port, "fs-rm", port.wait_for("FS RM name=DEMO.TXT rc=0", 5.0),
                "FS RM name=DEMO.TXT rc=0")
    port.clear()
    port.send("fs ls")
    port.read_for(1.0)
    ok &= check(port, "fs-rm-gone", "FS f=DEMO.TXT" not in port.text(), "DEMO.TXT absent")

    port.clear()
    port.send("fs format")
    ok &= check(port, "fs-format", port.wait_for("FS format=ok rc=0", 15.0), "FS format=ok rc=0")
    port.clear()
    port.send("fs ls")
    ok &= check(port, "fs-format-empty", port.wait_for("FS count=0", 5.0), "FS count=0")
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mode", choices=["basic", "fs", "watchdog", "fault"], default="basic")
    args = parser.parse_args()

    port = Port(args.port, args.baud)
    try:
        port.read_for(0.3)
        port.clear()
        if args.mode == "basic":
            ok = mode_basic(port)
        elif args.mode == "fs":
            ok = mode_fs(port)
        elif args.mode == "watchdog":
            ok = mode_watchdog(port)
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
