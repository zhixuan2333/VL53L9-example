#!/usr/bin/env bash
# Load an ELF into on-chip SRAM and run it (development boot, no signing, no flash).
# Board: BOOT1 switch at position 2-3 (dev mode), then power-cycle/reset.
# Usage: scripts/load-ram.sh path/to/app.elf
set -euo pipefail
ELF=${1:?usage: $0 app.elf}
CFG="$(cd "$(dirname "$0")/.." && pwd)/openocd/nucleo-n657x0.cfg"
ENTRY=$(arm-none-eabi-readelf -h "$ELF" | awk '/Entry point/ {print $4}')
ENTRY=$(printf '0x%08x' $((ENTRY & ~1)))  # strip Thumb bit; T is set via xPSR

openocd -f "$CFG" \
  -c "init" \
  -c "reset halt" \
  -c "load_image $ELF" \
  -c "reg xPSR 0x01000000" \
  -c "reg primask 0" \
  -c "reg pc $ENTRY" \
  -c "resume" \
  -c "exit"
echo "Running from SRAM (entry $ENTRY). It is lost on reset/power-off."
