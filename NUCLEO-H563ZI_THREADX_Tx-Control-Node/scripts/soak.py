#!/usr/bin/env python3
"""Soak test for Tx-Control-Node: watch uptime, watchdogs, control loop and stacks."""

import argparse
import csv
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from serial_check import Port, parse_ctrl  # noqa: E402

STATUS_RE = re.compile(
    r"STATUS ver=(\S+) uptime_ms=(\d+) hal_tick=(\d+) reset=(\w+) "
    r"safe=(\d) wdg_feed=(\d+) hb=([\d,]+) drop=(\d+) rx_drop=(\d+)"
)
STACK_RE = re.compile(r"STACK (.+)")
FAULT_SEQ_RE = re.compile(r"FAULT seq=")
FAULT_GUARD_RE = re.compile(r"FAULT guard=")
JIT_RE = re.compile(r"jit_us=([0-9.]+)/([0-9.]+)")


def parse_status(line):
    m = STATUS_RE.search(line)
    if not m:
        return None
    return {
        "ver": m.group(1),
        "uptime_ms": int(m.group(2)),
        "hal_tick": int(m.group(3)),
        "reset": m.group(4),
        "safe": int(m.group(5)),
        "wdg_feed": int(m.group(6)),
        "hb": [int(x) for x in m.group(7).split(",")],
        "drop": int(m.group(8)),
        "rx_drop": int(m.group(9)),
    }


def parse_stack(line):
    m = STACK_RE.search(line)
    if not m:
        return None
    usage = {}
    for item in m.group(1).split():
        if "=" not in item:
            continue
        name, pair = item.split("=", 1)
        used, size = pair.split("/", 1)
        usage[name] = (int(used), int(size))
    return usage


def read_lines(port, pending):
    pending += port.buf.decode("utf-8", "replace")
    port.clear()
    *lines, pending = pending.split("\n")
    return [line.rstrip("\r") for line in lines], pending


class Soak:
    def __init__(self, port, seconds, interval, stack_interval, setpoint, csv_path):
        self.port = port
        self.seconds = seconds
        self.interval = interval
        self.stack_interval = stack_interval
        self.setpoint = setpoint
        self.csv_path = csv_path
        self.pending = ""
        self.failures = []
        self.boots = 0
        self.faults = 0
        self.watchdog_events = 0
        self.status_count = 0
        self.stack_count = 0
        self.worst_jit_spread = 0.0
        self.bad_jit = 0
        self.base_status = None
        self.last_status = None
        self.base_ctrl = None
        self.last_ctrl = None
        self.base_stack = None
        self.worst_stack = {}
        self.csv = None
        self.csv_writer = None
        if csv_path:
            self.csv = open(csv_path, "w", newline="", encoding="utf-8")
            self.csv_writer = csv.writer(self.csv)
            self.csv_writer.writerow(
                [
                    "t_s",
                    "uptime_ms",
                    "hal_tick",
                    "reset",
                    "safe",
                    "wdg_feed",
                    "hb0",
                    "hb1",
                    "hb2",
                    "hb3",
                    "hb4",
                    "hb5",
                    "drop",
                    "rx_drop",
                    "cnt",
                    "ovr",
                    "jit_min_us",
                    "jit_max_us",
                    "err",
                ]
            )

    def fail(self, reason):
        self.failures.append(reason)
        print(f"FAIL {reason}", flush=True)

    def setup(self):
        self.port.drain_quiet()
        for command in ("fault clear", "plant sim", "ctrl on", f"set {self.setpoint}"):
            self.port.send(command)
            self.port.read_for(0.15)
        time.sleep(1.0)

        self.port.clear()
        self.port.send("status")
        if not self.port.wait_for("STATUS ver=", 3.0):
            self.fail("no STATUS after setup")
            return False
        self.port.read_for(0.1)
        for line in self.port.text().splitlines():
            status = parse_status(line)
            if status:
                self.base_status = status
                self.last_status = status
        self.port.clear()

        self.port.send("fault")
        if not self.port.wait_for("FAULT none", 2.0):
            self.fail("fault record present after setup")
            return False
        self.port.read_for(0.05)
        self.port.clear()

        self.port.send("stack")
        if not self.port.wait_for("STACK ", 2.0):
            self.fail("no STACK after setup")
            return False
        self.port.read_for(0.1)
        for line in self.port.text().splitlines():
            stack = parse_stack(line)
            if stack:
                self.base_stack = stack
                self.worst_stack = dict(stack)
        self.port.clear()

        self.port.clear()
        if self.port.wait_for("CTRL mode=", 2.0):
            self.port.read_for(0.1)
            for line in self.port.text().splitlines():
                ctrl = parse_ctrl(line)
                if ctrl:
                    self.base_ctrl = ctrl
                    self.last_ctrl = ctrl
        self.port.clear()

        if self.base_status is None:
            self.fail("setup: STATUS missing")
            return False
        if self.base_ctrl is None:
            self.fail("setup: CTRL missing")
            return False
        if self.base_ctrl["on"] != 1:
            self.fail(f"setup: control off (on={self.base_ctrl['on']})")
        if self.base_ctrl["mode"] != "sim":
            self.fail(f"setup: unexpected plant mode={self.base_ctrl['mode']}")
        if self.base_ctrl["sp"] != self.setpoint:
            self.fail(f"setup: sp={self.base_ctrl['sp']} expected {self.setpoint}")
        if self.base_status["safe"] != 0:
            self.fail("setup: safe mode active")
        if self.base_status["drop"] != 0 or self.base_status["rx_drop"] != 0:
            self.fail(
                f"setup: drops drop={self.base_status['drop']} "
                f"rx_drop={self.base_status['rx_drop']}"
            )
        return not self.failures

    def handle_status(self, status, now):
        self.status_count += 1
        old = self.last_status
        if status["uptime_ms"] < old["uptime_ms"]:
            self.fail(f"uptime went backwards {old['uptime_ms']} -> {status['uptime_ms']}")
        if status["reset"] != self.base_status["reset"]:
            self.fail(f"reset reason changed {self.base_status['reset']} -> {status['reset']}")
        if status["safe"] != 0:
            self.fail("safe mode active")
        if status["drop"] != 0 or status["rx_drop"] != 0:
            self.fail(f"console drops drop={status['drop']} rx_drop={status['rx_drop']}")
        if status["wdg_feed"] < old["wdg_feed"]:
            self.fail(f"wdg_feed went backwards {old['wdg_feed']} -> {status['wdg_feed']}")
        if len(status["hb"]) != len(old["hb"]):
            self.fail("heartbeat vector length changed")
        else:
            for i, (before, after) in enumerate(zip(old["hb"], status["hb"])):
                if after < before:
                    self.fail(f"hb[{i}] went backwards {before} -> {after}")
        elapsed_ms = (now - self._last_status_time) * 1000.0
        if elapsed_ms > 1500.0:
            advanced = status["uptime_ms"] - old["uptime_ms"]
            if advanced < elapsed_ms * 0.7:
                self.fail(f"uptime advance too small {advanced} ms for {elapsed_ms:.0f} ms")
        self.last_status = status
        self._last_status_time = now
        if self.csv_writer:
            ctrl = self.last_ctrl or {}
            self.csv_writer.writerow(
                [
                    f"{now - self._start:.1f}",
                    status["uptime_ms"],
                    status["hal_tick"],
                    status["reset"],
                    status["safe"],
                    status["wdg_feed"],
                    *status["hb"],
                    status["drop"],
                    status["rx_drop"],
                    ctrl.get("cnt", ""),
                    ctrl.get("ovr", ""),
                    ctrl.get("jit_min", ""),
                    ctrl.get("jit_max", ""),
                    ctrl.get("err", ""),
                ]
            )
            self.csv.flush()

    def handle_ctrl(self, ctrl):
        old = self.last_ctrl
        if old is not None:
            if ctrl["cnt"] < old["cnt"]:
                self.fail(f"control cnt went backwards {old['cnt']} -> {ctrl['cnt']}")
            if ctrl["ovr"] != 0:
                self.fail(f"control overrun ovr={ctrl['ovr']}")
        if ctrl["on"] != 1:
            self.fail("control turned off")
        if ctrl["mode"] != "sim":
            self.fail(f"plant mode changed to {ctrl['mode']}")
        if ctrl["sp"] != self.setpoint:
            self.fail(f"setpoint changed to {ctrl['sp']}")
        spread = ctrl["jit_max"] - ctrl["jit_min"]
        self.worst_jit_spread = max(self.worst_jit_spread, spread)
        if spread > 5.0:
            self.bad_jit += 1
            if self.bad_jit <= 3:
                self.fail(f"jit spread {spread:.2f} us (> 5 us)")
        self.last_ctrl = ctrl

    def handle_stack(self, stack):
        self.stack_count += 1
        for name, (used, size) in stack.items():
            if size == 0:
                continue
            ratio = used / size
            if name not in self.worst_stack or used > self.worst_stack[name][0]:
                self.worst_stack[name] = (used, size)
            if ratio > 0.80:
                self.fail(f"stack {name} usage {used}/{size} > 80%")

    def handle_line(self, line, now):
        if not line:
            return
        if line.startswith("BOOT "):
            self.boots += 1
            self.fail(f"unexpected boot: {line}")
            return
        if line.startswith("WATCHDOG "):
            self.watchdog_events += 1
            self.fail(f"watchdog event: {line}")
            return
        if FAULT_SEQ_RE.search(line) or FAULT_GUARD_RE.search(line):
            self.faults += 1
            self.fail(f"fault record: {line}")
            return
        status = parse_status(line)
        if status:
            self.handle_status(status, now)
            return
        ctrl = parse_ctrl(line)
        if ctrl:
            self.handle_ctrl(ctrl)
            return
        stack = parse_stack(line)
        if stack:
            self.handle_stack(stack)

    def run(self):
        if not self.setup():
            return False
        self._start = time.monotonic()
        self._last_status_time = self._start
        deadline = self._start + self.seconds
        next_status = self._start + 0.2
        next_stack = self._start + self.stack_interval
        print(
            f"SOAK start seconds={self.seconds} interval={self.interval}s "
            f"sp={self.setpoint}",
            flush=True,
        )
        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            self.port.pump(min(0.1, max(0.0, deadline - now)))
            lines, self.pending = read_lines(self.port, self.pending)
            for line in lines:
                self.handle_line(line, now)
            if now >= next_status:
                self.port.send("status")
                next_status += self.interval
                if next_status <= now:
                    next_status = now + self.interval
            if now >= next_stack:
                self.port.send("stack")
                next_stack += self.stack_interval
                if next_stack <= now:
                    next_stack = now + self.stack_interval
        return self.finish()

    def finish(self):
        self.port.read_for(0.5)
        lines, self.pending = read_lines(self.port, self.pending)
        for line in lines:
            self.handle_line(line, time.monotonic())

        self.port.drain_quiet(quiet=0.2, max_total=1.0)
        self.port.send("status")
        self.port.wait_for("STATUS ver=", 2.0)
        self.port.read_for(0.15)
        final_status = None
        for line in self.port.text().splitlines():
            status = parse_status(line)
            if status:
                final_status = status
        self.port.clear()

        self.port.send("stack")
        self.port.wait_for("STACK ", 2.0)
        self.port.read_for(0.1)
        final_stack = None
        for line in self.port.text().splitlines():
            stack = parse_stack(line)
            if stack:
                final_stack = stack
                self.handle_stack(stack)
        self.port.clear()

        self.port.send("fault")
        self.port.wait_for("FAULT", 2.0)
        self.port.read_for(0.05)
        fault_text = self.port.text()
        if "FAULT none" not in fault_text:
            self.fail(f"final fault not none: {fault_text.strip()!r}")
        self.port.clear()

        self.port.send("wdg")
        self.port.wait_for("WATCHDOG", 2.0)
        self.port.clear()

        if final_status is None:
            final_status = self.last_status
        if final_stack is None:
            self.fail("final STACK missing")

        elapsed_s = time.monotonic() - self._start
        if final_status and self.base_status:
            uptime_delta = final_status["uptime_ms"] - self.base_status["uptime_ms"]
            if uptime_delta < elapsed_s * 1000.0 * 0.7:
                self.fail(
                    f"uptime delta {uptime_delta} ms < 70% of elapsed {elapsed_s * 1000.0:.0f} ms"
                )
            if final_status["wdg_feed"] <= self.base_status["wdg_feed"]:
                self.fail("wdg_feed did not advance")
            for i, (before, after) in enumerate(
                zip(self.base_status["hb"], final_status["hb"])
            ):
                if after <= before:
                    self.fail(f"hb[{i}] did not advance {before} -> {after}")
        if final_status and final_status["safe"] != 0:
            self.fail("final safe mode active")
        if final_status and final_status["drop"] != 0:
            self.fail("final drop non-zero")
        if final_status and final_status["rx_drop"] != 0:
            self.fail("final rx_drop non-zero")

        if self.base_ctrl and self.last_ctrl:
            cnt_delta = self.last_ctrl["cnt"] - self.base_ctrl["cnt"]
            if cnt_delta < self.seconds * 10000 * 0.7:
                self.fail(
                    f"control cnt delta {cnt_delta} < 70% of expected "
                    f"{self.seconds * 10000}"
                )
            if abs(self.last_ctrl["err"]) > 10:
                self.fail(f"final control err={self.last_ctrl['err']}")
            if abs(self.last_ctrl["fb"] - self.setpoint) > 50:
                self.fail(
                    f"final feedback {self.last_ctrl['fb']} far from sp {self.setpoint}"
                )

        if self.csv:
            self.csv.close()

        if self.failures:
            print(
                f"SOAK FAIL seconds={elapsed_s:.1f} status={self.status_count} "
                f"boots={self.boots} faults={self.faults} "
                f"watchdog={self.watchdog_events} failures={len(self.failures)}",
                flush=True,
            )
            return False

        print(
            f"SOAK PASS seconds={elapsed_s:.1f} status={self.status_count} "
            f"boots=0 faults=0 resets=0 safe=0 drops=0",
            flush=True,
        )
        if final_status:
            print(
                f"  uptime_ms={final_status['uptime_ms']} "
                f"wdg_feed={final_status['wdg_feed']} "
                f"hb={','.join(str(x) for x in final_status['hb'])}",
                flush=True,
            )
        if self.last_ctrl:
            print(
                f"  ctrl cnt={self.last_ctrl['cnt']} ovr={self.last_ctrl['ovr']} "
                f"err={self.last_ctrl['err']} fb={self.last_ctrl['fb']} "
                f"worst_jit_spread={self.worst_jit_spread:.2f}us",
                flush=True,
            )
        if self.worst_stack:
            print(
                "  stack "
                + " ".join(
                    f"{name}={used}/{size}"
                    for name, (used, size) in sorted(self.worst_stack.items())
                ),
                flush=True,
            )
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--seconds", type=int, default=3600)
    parser.add_argument("--interval", type=int, default=10)
    parser.add_argument("--stack-interval", type=int, default=300)
    parser.add_argument("--setpoint", type=int, default=3000)
    parser.add_argument("--csv", default=None)
    args = parser.parse_args()

    port = Port(args.port, 115200)
    try:
        soak = Soak(
            port,
            args.seconds,
            args.interval,
            args.stack_interval,
            args.setpoint,
            args.csv,
        )
        return 0 if soak.run() else 1
    finally:
        port.close()


if __name__ == "__main__":
    raise SystemExit(main())
