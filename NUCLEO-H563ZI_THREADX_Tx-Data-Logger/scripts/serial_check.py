#!/usr/bin/env python3
"""Serial assertions for the Tx-Data-Logger firmware."""

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


# "target=" sits between mode and rate; the non-capturing group keeps the
# numbering below unchanged.
ACQ_INFO = (
    r"ACQ running=(\d) mode=(\w+) (?:target=\w+ )?rate=(\d+) ch=(\d+) file=(\S+) "
    r"windows=(\d+) "
    r"samples=(\d+) files=(\d+) min=(\d+) max=(\d+) drop=(\d+) dma_ovr=(\d+) "
    r"qdrop=(\d+) flush=(\d+)"
)


def acq_capture(port, label, start_cmd, seconds, min_windows, max_drop_ratio,
                max_ovr=0, min_files=2):
    """Start a capture, let it run, stop it and assert the counters."""
    port.clear()
    port.send(start_cmd)
    ok = check(port, f"{label}-start", port.wait_for("ACQ start=ok", 5.0), "ACQ start=ok")

    port.read_for(seconds)

    port.clear()
    port.send("acq stop")
    ok &= check(port, f"{label}-stop", port.wait_for("ACQ stop=ok", 5.0), "ACQ stop=ok")

    port.clear()
    port.send("acq info")
    port.read_for(0.6)
    text = port.text()
    match = re.search(ACQ_INFO, text)
    ok &= check(port, f"{label}-stats", match is not None, "ACQ running=... line")
    if not match:
        return ok

    mode = match.group(2)
    channels = int(match.group(4))
    windows = int(match.group(6))
    samples = int(match.group(7))
    files = int(match.group(8))
    low = int(match.group(9))
    high = int(match.group(10))
    drops = int(match.group(11))
    dma_ovr = int(match.group(12))
    q_drops = int(match.group(13))

    ok &= check(port, f"{label}-windows", windows >= min_windows,
                f"windows>={min_windows} (got {windows})")
    ok &= check(port, f"{label}-samples", samples == windows * 500 * channels,
                "samples == windows*500*channels")
    ok &= check(port, f"{label}-no-overrun", dma_ovr <= max_ovr,
                f"dma_ovr<={max_ovr} (got {dma_ovr})")
    ok &= check(port, f"{label}-no-queue-drop", q_drops == 0, f"qdrop=0 (got {q_drops})")
    ok &= check(port, f"{label}-drops",
                drops <= max(windows * 500 * channels * max_drop_ratio, 1),
                f"drop<={max_drop_ratio:.0%} of samples (got {drops})")
    ok &= check(port, f"{label}-adc-range", (high <= 4095) and (high >= low),
                "0 <= min <= max <= 4095")
    ok &= check(port, f"{label}-rotated", files >= min_files,
                f"files>={min_files} (got {files})")
    ok &= check(port, f"{label}-mode", mode in ("irq", "dma"), f"mode in irq/dma (got {mode})")

    port.clear()
    port.send("acq check")
    ok &= check(port, f"{label}-check", port.wait_for("ACQ CHECK PASS", 8.0), "ACQ CHECK PASS")
    return ok


def mode_acq(port):
    port.drain_quiet()

    port.clear()
    port.send("acq ch 1")
    ok = check(port, "acq-single-channel", port.wait_for("ACQ channels=1", 3.0), "ACQ channels=1")

    port.clear()
    port.send("fs format")
    ok &= check(port, "acq-fresh-volume", port.wait_for("FS format=ok rc=0", 15.0),
                "FS format=ok rc=0")

    # Interrupt-driven baseline: one sample per ADC end-of-conversion.
    ok &= acq_capture(port, "acq-irq", "acq start irq 10000", 9.0, 120, 0.0, 0, 1)

    port.clear()
    port.send("fs ls")
    ok &= check(port, "acq-file-0", port.wait_for("FS f=LOG0000.CSV", 5.0), "FS f=LOG0000.CSV")

    port.clear()
    port.send("fs format")
    ok &= check(port, "acq-cleanup", port.wait_for("FS format=ok rc=0", 15.0),
                "FS format=ok rc=0")

    # GPDMA path at 5x the interrupt rate.
    ok &= acq_capture(port, "acq-dma", "acq start dma 50000", 4.0, 120, 0.0)

    port.clear()
    port.send("fs format")
    ok &= check(port, "acq-dma-cleanup", port.wait_for("FS format=ok rc=0", 15.0),
                "FS format=ok rc=0")

    # 10x the interrupt rate: only possible because the sampler no longer
    # writes; a couple of DMA halves may still be lost to scheduler latency.
    ok &= acq_capture(port, "acq-100k", "acq start dma 100000", 4.0, 500, 0.01, 16)

    port.clear()
    port.send("fs format")
    ok &= check(port, "acq-100k-cleanup", port.wait_for("FS format=ok rc=0", 15.0),
                "FS format=ok rc=0")
    return ok


def mode_mch(port):
    """Multi-channel: deterministic de-interleave plus 2- and 4-channel runs."""
    port.drain_quiet()

    port.clear()
    port.send("acq demux")
    ok = check(port, "mch-demux", port.wait_for("ACQ DEMUX PASS", 5.0), "ACQ DEMUX PASS")
    port.read_for(0.3)
    text = port.text()
    ok &= check(port, "mch-demux-ch3", "ACQ DEMUX ch=3 min=ok max=ok avg=ok" in text,
                "ACQ DEMUX ch=3 all ok")

    # One trigger converts the whole sequence faster than the EOC ISR can read
    # DR, so multi-channel must be refused on the interrupt path.
    port.clear()
    port.send("acq ch 2")
    ok &= check(port, "mch-ch2-set", port.wait_for("ACQ channels=2", 3.0), "ACQ channels=2")
    port.clear()
    port.send("acq start irq 10000")
    ok &= check(port, "mch-irq-refused",
                port.wait_for("ACQ ERR multichannel-requires-dma", 5.0),
                "ACQ ERR multichannel-requires-dma")

    for ch, seconds in ((2, 4.0), (4, 4.0)):
        port.clear()
        port.send(f"acq ch {ch}")
        ok &= check(port, f"mch-ch{ch}-set", port.wait_for(f"ACQ channels={ch}", 3.0),
                    f"ACQ channels={ch}")
        port.clear()
        port.send("fs format")
        ok &= check(port, f"mch-ch{ch}-volume", port.wait_for("FS format=ok rc=0", 15.0),
                    "FS format=ok rc=0")
        ok &= acq_capture(port, f"mch-ch{ch}", "acq start dma 50000", seconds, 80, 0.01, 16)

        port.clear()
        port.send("fs read LOG0000.CSV")
        ok &= check(port, f"mch-ch{ch}-csv", port.wait_for(f",500,{ch},", 5.0),
                    f",500,{ch},")

    port.clear()
    port.send("acq ch 1")
    ok &= check(port, "mch-restore", port.wait_for("ACQ channels=1", 3.0), "ACQ channels=1")

    port.clear()
    port.send("fs format")
    ok &= check(port, "mch-cleanup", port.wait_for("FS format=ok rc=0", 15.0),
                "FS format=ok rc=0")
    return ok


def mode_dma(port):
    port.drain_quiet()

    port.clear()
    port.send("dma env")
    ok = check(port, "dma-env-sau", port.wait_for("DMA ENV sau_ctrl=", 5.0), "DMA ENV sau_ctrl=")
    ok &= check(port, "dma-env-mpcbb", port.wait_for("DMA ENV src=", 3.0), "DMA ENV src=")

    port.clear()
    port.send("dma test")
    ok &= check(port, "dma-selftest", port.wait_for("DMA MEMTEST PASS", 10.0),
                "DMA MEMTEST PASS")
    port.read_for(0.4)
    text = port.text()
    # Fully attributed transfer must move data; channel-level attributes alone
    # must not (this is the bug the matrix documents).
    ok &= check(port, "dma-case-secure", "attr=PSSD port=p0/p1 buf=sram1" in text and
                "mism=00" in text, "PSSD sram1 mism=00")
    ok &= check(port, "dma-case-ps-blocked", "attr=PS port=p0/p1" in text,
                "attr=PS port=p0/p1")
    ok &= check(port, "dma-result", port.wait_for("DMA RESULT PASS", 3.0), "DMA RESULT PASS")

    # The acquisition path must survive the controller being poked.
    port.clear()
    port.send("fs format")
    ok &= check(port, "dma-volume", port.wait_for("FS format=ok rc=0", 15.0),
                "FS format=ok rc=0")
    ok &= acq_capture(port, "dma-after-test", "acq start dma 50000", 3.0, 80, 0.02)

    port.clear()
    port.send("fs format")
    ok &= check(port, "dma-cleanup", port.wait_for("FS format=ok rc=0", 15.0),
                "FS format=ok rc=0")
    return ok


def mode_rtc(port):
    """Calendar clock and the CSV v2 timestamps it produces."""
    port.drain_quiet()

    port.clear()
    port.send("rtc")
    ok = check(port, "rtc-ready", port.wait_for("RTC ready=1", 4.0), "RTC ready=1")
    text = port.text()
    ok &= check(port, "rtc-source", ("source=lse" in text) or ("source=lsi" in text),
                "source=lse|lsi")

    port.clear()
    port.send("rtc set 2026-01-02 03:04:05")
    ok &= check(port, "rtc-set", port.wait_for("RTC set rc=0", 4.0), "RTC set rc=0")
    ok &= check(port, "rtc-echo", port.wait_for("2026-01-02 03:04:0", 3.0),
                "2026-01-02 03:04:0x")

    port.clear()
    port.send("rtc")
    port.read_for(1.2)
    text = port.text()
    ok &= check(port, "rtc-advances", "2026-01-02 03:04:" in text, "2026-01-02 03:04:")

    port.clear()
    port.send("rtc set 2026-13-45 99:99:99")
    ok &= check(port, "rtc-reject-bad", port.wait_for("RTC set rc=-2", 4.0),
                "RTC set rc=-2")

    port.send("acq target sim")
    port.wait_for("ACQ target=", 3.0)
    port.clear()
    port.send("acq ch 2")
    ok &= check(port, "rtc-ch2", port.wait_for("ACQ channels=2", 3.0), "ACQ channels=2")
    port.clear()
    port.send("fs format")
    ok &= check(port, "rtc-volume", port.wait_for("FS format=ok rc=0", 20.0),
                "FS format=ok rc=0")
    ok &= acq_capture(port, "rtc-cap", "acq start dma 10000", 1.5, 20, 0.01, 0, 1)

    port.clear()
    port.send("acq check")
    ok &= check(port, "rtc-check", port.wait_for("ACQ CHECK PASS", 10.0), "ACQ CHECK PASS")
    text = port.text()
    ok &= check(port, "rtc-header", "hdr=1" in text, "hdr=1")
    ok &= check(port, "rtc-monotonic", "bad_ts=0" in text, "bad_ts=0")
    ok &= check(port, "rtc-year", "2026-01-02" in text, "2026-01-02")

    port.clear()
    port.send("acq ch 1")
    ok &= check(port, "rtc-restore", port.wait_for("ACQ channels=1", 3.0), "ACQ channels=1")
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mode",
                        choices=["basic", "fs", "acq", "mch", "dma", "rtc", "watchdog", "fault"],
                        default="basic")
    args = parser.parse_args()

    port = Port(args.port, args.baud)
    try:
        port.read_for(0.3)
        port.clear()
        if args.mode == "basic":
            ok = mode_basic(port)
        elif args.mode == "fs":
            ok = mode_fs(port)
        elif args.mode == "acq":
            ok = mode_acq(port)
        elif args.mode == "mch":
            ok = mode_mch(port)
        elif args.mode == "dma":
            ok = mode_dma(port)
        elif args.mode == "rtc":
            ok = mode_rtc(port)
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
