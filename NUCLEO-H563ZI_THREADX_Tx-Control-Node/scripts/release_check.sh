#!/usr/bin/env bash
# Build/flash the release profile and run non-destructive release checks.
set -euo pipefail

cd "$(dirname "$0")/.."

PORT=${PORT:-/dev/ttyACM0}
TOOLCHAIN=${TOOLCHAIN_PATH:-/opt/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin}
SIZE="$TOOLCHAIN/arm-none-eabi-size"
NM="$TOOLCHAIN/arm-none-eabi-nm"

make clean
make -j"$(nproc)" APP_PROFILE=release

ELF=$(find . -maxdepth 1 -name '*.elf' -print -quit)
for sym in app_control_tim_isr app_console_status app_fault_capture app_watchdog_poll; do
  if ! "$NM" "$ELF" | grep " $sym$" > /dev/null; then
    echo "FAIL symbol-missing: $sym"
    exit 1
  fi
done
"$SIZE" "$ELF"
echo "PASS release-build"

make flash APP_PROFILE=release
python3 scripts/serial_check.py --port "$PORT" --mode release
python3 scripts/serial_check.py --port "$PORT" --mode control
python3 scripts/serial_check.py --port "$PORT" --mode param
python3 scripts/soak.py --port "$PORT" --seconds 60 --interval 5 --stack-interval 20

echo "RELEASE CHECK PASS"
