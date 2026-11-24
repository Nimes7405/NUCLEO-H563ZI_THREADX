#!/usr/bin/env bash
# Build, flash and run serial self-checks for Tx-Usb-Data-Logger.
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

# Exit code 2 means the assertion set was skipped, not that it failed. Only the
# usb group does that, and only when the host cable is absent; STRICT=1 turns
# the skip into a failure so a release run cannot pass without the cable.
run_group() {
  local rc=0
  python3 scripts/serial_check.py --port "$PORT" --mode "$1" || rc=$?
  if [ "$rc" -eq 2 ]; then
    if [ "${STRICT:-0}" = "1" ]; then
      echo "FAIL $1: skipped but STRICT=1"
      exit 1
    fi
    echo "SKIP $1"
    return 0
  fi
  return "$rc"
}

case "$MODE" in
  basic|fs|acq|mch|dma|rtc|usb|watchdog|fault)
    run_group "$MODE"
    ;;
  all)
    run_group basic
    run_group fs
    run_group acq
    run_group mch
    run_group dma
    run_group rtc
    run_group usb
    run_group watchdog
    run_group fault
    ;;
  *)
    echo "usage: $0 [basic|fs|acq|mch|dma|rtc|usb|watchdog|fault|all]" >&2
    exit 2
    ;;
esac
