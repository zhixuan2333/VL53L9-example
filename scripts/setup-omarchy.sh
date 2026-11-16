#!/usr/bin/env bash
# One-time host setup on omarchy (Arch Linux).
set -euo pipefail

sudo pacman -S --needed --noconfirm \
  arm-none-eabi-gcc arm-none-eabi-newlib arm-none-eabi-binutils arm-none-eabi-gdb \
  make git cmake python python-pyserial picocom usbutils

# Arch's packaged openocd (0.12.0) predates STM32N6 support (target/stm32n6x.cfg).
# Use openocd-git from the AUR (omarchy ships yay).
if ! { command -v openocd >/dev/null && openocd -c 'find target/stm32n6x.cfg; exit' >/dev/null 2>&1; }; then
  yay -S --needed --noconfirm openocd-git
fi

# probe-rs (official installer, into ~/.cargo/bin).
if ! command -v probe-rs >/dev/null; then
  curl --proto '=https' --tlsv1.2 -LsSf \
    https://github.com/probe-rs/probe-rs/releases/latest/download/probe-rs-tools-installer.sh | sh
fi

# ST-LINK udev rule so openocd works without sudo.
sudo tee /etc/udev/rules.d/49-stlink.rules >/dev/null <<'RULES'
SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", MODE="0666", TAG+="uaccess"
RULES
sudo udevadm control --reload-rules
sudo udevadm trigger

# Serial port access (/dev/ttyACM*).
sudo usermod -aG uucp "$USER"

echo "Done. Log out/in once so the uucp group applies."
