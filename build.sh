#!/usr/bin/env bash
# ============================================================
#  Build everything for carrr
#
#    ./build.sh            build firmware + android
#    ./build.sh fw         firmware only
#    ./build.sh app        android only
#
#  Firmware needs ESP-IDF (auto-sourced if not already in the env).
#  Android needs JAVA_HOME + ANDROID_HOME (see scripts/setup-*.sh).
#
#  Flashing is NOT done here - do it yourself:
#    cd firmware && idf.py -p /dev/ttyUSB0 -b 115200 flash monitor
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WHAT="${1:-all}"

FW_BIN="$ROOT/firmware/build/ble_rover.bin"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"

build_fw() {
    echo
    echo "==================== FIRMWARE ===================="
    if [ -z "${IDF_PATH:-}" ]; then
        local export_sh="$HOME/esp/esp-idf/export.sh"
        if [ ! -f "$export_sh" ]; then
            echo "ERROR: ESP-IDF not found at $export_sh"
            echo "       run scripts/setup-esp32-wsl.sh first"
            exit 1
        fi
        echo "sourcing $export_sh ..."
        # shellcheck disable=SC1090
        . "$export_sh" >/dev/null 2>&1
    fi

    cd "$ROOT/firmware"
    idf.py build
    echo "-> firmware built: $FW_BIN"
}

build_app() {
    echo
    echo "===================== ANDROID ====================="
    if [ -z "${ANDROID_HOME:-}" ]; then
        echo "ERROR: ANDROID_HOME is not set"
        echo "       run scripts/setup-android-wsl.sh + setup-shell-env.sh"
        exit 1
    fi
    if ! command -v gradle >/dev/null 2>&1 && [ ! -x ./gradlew ]; then
        echo "ERROR: neither gradle nor ./gradlew found"
        exit 1
    fi

    cd "$ROOT/android"
    echo "sdk.dir=$ANDROID_HOME" > local.properties
    if [ -x ./gradlew ]; then ./gradlew assembleDebug; else gradle assembleDebug; fi
    echo "-> apk built: $APK"
}

case "$WHAT" in
    all) build_fw; build_app ;;
    fw|firmware) build_fw ;;
    app|android) build_app ;;
    *) echo "usage: $0 [all|fw|app]"; exit 2 ;;
esac

echo
echo "======================== DONE ========================"
echo "firmware : $FW_BIN"
echo "apk      : $APK"
echo
echo "flash it yourself:"
echo "  cd firmware && idf.py -p /dev/ttyUSB0 -b 115200 flash monitor"
echo "install the app:"
echo "  adb install -r android/app/build/outputs/apk/debug/app-debug.apk"
