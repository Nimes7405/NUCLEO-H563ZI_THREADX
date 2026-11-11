#!/usr/bin/env python3
"""Repeat fault and watchdog reset checks on Tx-Control-Node."""

import argparse
import sys

from serial_check import Port


CASES = {
    "guard": (
        "guard",
        8.0,
        (
            ("guard-trigger", "GUARD triggering write to 0x"),
            ("fault-boot", "BOOT reset=software fault=yes"),
            ("guard-cfsr", "cfsr=0x00000082"),
            ("guard-record", "FAULT guard=prod_a"),
            ("ready", "READY"),
        ),
    ),
    "crash": (
        "crash",
        8.0,
        (
            ("crash-trigger", "CRASH triggering bus fault"),
            ("fault-boot", "BOOT reset=software fault=yes"),
            ("busfault-cfsr", "cfsr=0x00008200"),
            ("busfault-bfar", "bfar=0xfffffff0"),
            ("ready", "READY"),
        ),
    ),
    "iwdg": (
        "stall 3",
        10.0,
        (
            ("stall-detected", "WATCHDOG stall=control"),
            ("iwdg-reset", "BOOT reset=iwdg"),
            ("fault-none", "fault=none"),
            ("ready", "READY"),
        ),
    ),
    "wwdg": (
        "stall 4",
        5.0,
        (
            ("wwdg-reset", "BOOT reset=wwdg"),
            ("fault-none", "fault=none"),
            ("ready", "READY"),
        ),
    ),
}


def wait(port, label, needle, timeout):
    found = port.wait_for(needle, timeout)
    if found:
        print(f"PASS {label}")
    else:
        print(f"FAIL {label}: missing {needle!r}")
    return found


def run_case(port, name, trial):
    port.drain_quiet()
    port.send("fault clear")
    if not wait(port, f"{name}[{trial}] clear", "FAULT cleared", 2.0):
        return False

    command, timeout, assertions = CASES[name]
    port.clear()
    port.send(command)
    ok = True
    for label, expected in assertions:
        passed = wait(port, f"{name}[{trial}] {label}", expected, timeout)
        ok = passed and ok
        if not passed:
            return False
    port.drain_quiet()
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--guard", type=int, default=10, help="guard fault repetitions")
    parser.add_argument("--crash", type=int, default=10, help="BusFault repetitions")
    parser.add_argument("--iwdg", type=int, default=5, help="IWDG stall repetitions")
    parser.add_argument("--wwdg", type=int, default=10, help="WWDG stall repetitions")
    args = parser.parse_args()
    counts = {name: getattr(args, name) for name in CASES}
    if any(count <= 0 for count in counts.values()):
        parser.error("all repetition counts must be greater than zero")

    port = Port(args.port, args.baud)
    try:
        port.read_for(0.3)
        port.clear()
        port.send("status")
        if not wait(port, "dev-profile", "profile=dev", 3.0):
            print(port.text()[-3000:])
            return 1

        for name, count in counts.items():
            for trial in range(1, count + 1):
                if not run_case(port, name, trial):
                    print("\n--- captured output ---")
                    print(port.text()[-3000:])
                    return 1
                print(f"PASS {name} trial={trial}/{count}")
    finally:
        port.close()

    print("FAULT MATRIX PASS " + " ".join(f"{name}={count}" for name, count in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
