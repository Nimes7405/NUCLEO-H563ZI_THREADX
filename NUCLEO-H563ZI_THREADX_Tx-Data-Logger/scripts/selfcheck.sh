#!/usr/bin/env bash
# Build, flash and run serial self-checks for Tx-Data-Logger.
set -euo pipefail

cd "$(dirname "$0")/.."

PORT=${PORT:-/dev/ttyACM0}
MODE=${1:-basic}
TOOLCHAIN=${TOOLCHAIN_PATH:-/opt/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin}
NM="$TOOLCHAIN/arm-none-eabi-nm"

make clean
make -j"$(nproc)"

ELF=$(find . -maxdepth 1 -name '*.elf' -print -quit)
for sym in app_fault_capture HardFault_Handler BusFault_Handler app_watchdog_poll app_log_printf app_fxdisk_write_pattern app_acq_check app_dma_mem_selftest _fx_media_open _lx_nor_flash_sector_write; do
  if ! "$NM" "$ELF" | grep " $sym$" > /dev/null; then
    echo "FAIL symbol-missing: $sym"
    exit 1
  fi
done
echo "PASS symbols"

make flash

case "$MODE" in
  basic|fs|acq|mch|dma|watchdog|fault)
    python3 scripts/serial_check.py --port "$PORT" --mode "$MODE"
    ;;
  all)
    python3 scripts/serial_check.py --port "$PORT" --mode basic
    python3 scripts/serial_check.py --port "$PORT" --mode fs
    python3 scripts/serial_check.py --port "$PORT" --mode acq
    python3 scripts/serial_check.py --port "$PORT" --mode mch
    python3 scripts/serial_check.py --port "$PORT" --mode dma
    python3 scripts/serial_check.py --port "$PORT" --mode watchdog
    python3 scripts/serial_check.py --port "$PORT" --mode fault
    ;;
  *)
    echo "usage: $0 [basic|fs|acq|mch|dma|watchdog|fault|all]" >&2
    exit 2
    ;;
esac
