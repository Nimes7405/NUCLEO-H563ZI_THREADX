#!/usr/bin/env bash
# Build, flash and run serial self-checks for Tx-Reliability-Monitor.
set -euo pipefail

cd "$(dirname "$0")/.."

PORT=${PORT:-/dev/ttyACM0}
MODE=${1:-basic}
OPT=${OPT:--O0}
APP_PROFILE=${APP_PROFILE:-dev}
TOOLCHAIN=${TOOLCHAIN_PATH:-/opt/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin}
NM="$TOOLCHAIN/arm-none-eabi-nm"

make clean
make -j"$(nproc)" OPT="$OPT" APP_PROFILE="$APP_PROFILE"

ELF=$(find . -maxdepth 1 -name '*.elf' -print -quit)
for sym in app_fault_capture HardFault_Handler BusFault_Handler app_watchdog_poll app_log_printf; do
  if ! "$NM" "$ELF" | grep " $sym$" > /dev/null; then
    echo "FAIL symbol-missing: $sym"
    exit 1
  fi
done
echo "PASS symbols"

make flash OPT="$OPT" APP_PROFILE="$APP_PROFILE"

case "$MODE" in
  basic|control|mpu|wwdg|watchdog|fault)
    python3 scripts/serial_check.py --port "$PORT" --mode "$MODE"
    ;;
  all)
    python3 scripts/serial_check.py --port "$PORT" --mode basic
    python3 scripts/serial_check.py --port "$PORT" --mode control
    python3 scripts/serial_check.py --port "$PORT" --mode mpu
    python3 scripts/serial_check.py --port "$PORT" --mode wwdg
    python3 scripts/serial_check.py --port "$PORT" --mode watchdog
    python3 scripts/serial_check.py --port "$PORT" --mode fault
    ;;
  *)
    echo "usage: $0 [basic|control|mpu|wwdg|watchdog|fault|all]" >&2
    exit 2
    ;;
esac
