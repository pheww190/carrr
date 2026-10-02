#!/usr/bin/env bash
# ============================================================
#  Slim ESP-IDF setup for ESP32 on WSL2 / Debian
#  Board : ESP32-WROOM-32 (DOIT DevKit V1 clone)
#  Target: esp32            (Xtensa LX6, original ESP32)
#  Installs ONLY the Xtensa toolchain for the esp32 target,
#  not all 8 chips, so it stays ~1.3 GB instead of ~9 GB.
#
#  Usage:  bash setup-esp32-wsl.sh
# ============================================================
set -euo pipefail

IDF_VERSION="v5.5.5"          # v5.x so the delivered firmware builds as-is
IDF_DIR="$HOME/esp/esp-idf"
PORT="${1:-/dev/ttyUSB0}"

echo "== 1/7  apt prerequisites =="
sudo apt update
sudo apt install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 usbutils

echo "== 2/7  clone ESP-IDF ($IDF_VERSION, shallow) =="
mkdir -p "$HOME/esp"
if [ ! -d "$IDF_DIR" ]; then
  git clone -b "$IDF_VERSION" --depth 1 \
    --shallow-submodules --recurse-submodules \
    https://github.com/espressif/esp-idf.git "$IDF_DIR"
else
  echo "   already present, skipping clone"
fi

echo "== 3/7  install ESP32-only tools (xtensa; skips riscv/clang/other targets) =="
cd "$IDF_DIR"
./install.sh esp32

echo "== 4/7  trim other-chip prebuilt blobs (safe: esp32 target only) =="
rm -rf components/bt/controller/lib_esp32c2 \
       components/bt/controller/lib_esp32c3_family \
       components/bt/controller/lib_esp32c5 \
       components/bt/controller/lib_esp32c6 \
       components/bt/controller/lib_esp32h2 \
       components/bt/controller/lib_esp32h4 \
       components/bt/controller/lib_esp32s31 || true

echo "== 5/7  serial permissions (dialout) =="
sudo usermod -aG dialout "$USER"

echo "== 6/7  shell alias for the environment =="
grep -q "alias get_idf=" "$HOME/.bashrc" 2>/dev/null || \
  echo "alias get_idf='. \$HOME/esp/esp-idf/export.sh'" >> "$HOME/.bashrc"

echo "== 7/7  done =="
echo
echo "NOW DO THIS:"
echo "  1) From Windows PowerShell:  wsl --shutdown   then reopen WSL"
echo "  2) In WSL:  get_idf"
echo "  3) In WSL:  idf.py --version"
echo
echo "Then build the firmware:"
echo "  cd ~/ble_rover"
echo "  idf.py set-target esp32"
echo "  idf.py build"
echo "  idf.py -p $PORT flash monitor"
