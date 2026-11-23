#!/usr/bin/env python3
"""Analog front-end health check for the Tx-Usb-Data-Logger board.

Two stages:

  internal  no wiring needed. Validates the converter's own references and a
            DAC-driven pad loopback, which separates "converter is dead" from
            "the input path is dead".
  ext       needs 3V3 on A1 (PC0) and GND on A2 (PC3). Adds a real four-channel
            capture and checks that the two levels stay in separate bands.

Usage:
  ./scripts/adc_healthcheck.py                 # internal only
  ./scripts/adc_healthcheck.py --wired         # internal, then wait and run ext
  ./scripts/adc_healthcheck.py --wired --yes   # no prompt, assume wired
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from serial_check import Port  # noqa: E402


def run(port, command, needle, timeout):
    port.clear()
    port.send(command)
    end = time.time() + timeout
    while time.time() < end:
        port.pump(0.2)
        if needle in port.text():
            break
    time.sleep(0.3)
    port.pump(0.2)
    return port.text()


def lines_with(text, prefix="ADC HC"):
    return [l.strip() for l in text.splitlines() if prefix in l]


def report(title, rows):
    print("\n== %s ==" % title)
    if not rows:
        print("  (no output from the board)")
        return False
    ok = True
    for line in rows:
        print("  " + line)
        if " FAIL" in line:
            ok = False
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("PORT", "/dev/ttyACM0"))
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--wired", action="store_true",
                    help="also run the external stage (PC0=3V3, PC3=GND)")
    ap.add_argument("--yes", action="store_true",
                    help="with --wired, do not wait for confirmation")
    args = ap.parse_args()

    port = Port(args.port, args.baud)
    port.drain_quiet(quiet=0.5, max_total=4.0)

    # A capture in progress would make both stages refuse to run.
    run(port, "acq stop", "ACQ stop=", 20.0)

    text = run(port, "acq hwcheck", "ADC HC PASS", 30.0)
    good = report("stage 1: internal (no wiring)", lines_with(text))
    if "ADC HC PASS" not in text and "ADC HC FAIL" not in text:
        print("\nRESULT: UNKNOWN (board did not answer)")
        return 2

    if not args.wired:
        print("\nNext: fit 3V3 on A1 (PC0) and GND on A2 (PC3), then rerun")
        print("      with --wired for the four-channel separation test.")
        return 0 if good else 1

    if good and not args.yes:
        print("\nFit 3V3 on A1 (PC0) and GND on A2 (PC3), then press Enter...")
        try:
            input()
        except EOFError:
            pass
    elif not good and not args.yes:
        print("\nStage 1 already failed; the external stage will most likely")
        print("fail too. Press Enter to run it anyway, or Ctrl-C to stop...")
        try:
            input()
        except EOFError:
            pass

    text = run(port, "acq hwcheck ext", "ADC HC EXT", 40.0)
    ext = lines_with(text, "ADC HC EXT")
    good_ext = report("stage 2: external + four-channel separation", ext)

    print("\n================ RESULT ================")
    if good and good_ext:
        print("  ANALOG FRONT END OK")
        print("  VREFINT/temperature/VBAT sane, DAC loopback tracks,")
        print("  PC0 reads 3.3 V, PC3 reads 0 V, and the two levels stay")
        print("  in separate bands through the four-channel DMA path.")
        rc = 0
    elif not good:
        print("  ANALOG FRONT END FAILED (internal stage)")
        print("  The converter's own references or the DAC loopback is wrong,")
        print("  which points at the part itself rather than the wiring.")
        rc = 1
    else:
        print("  ANALOG FRONT END FAILED (external stage)")
        print("  References look fine but the pads do not follow the applied")
        print("  voltage: check the jumper wires first, then suspect the")
        print("  input path.")
        rc = 1
    print("========================================")
    port.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
