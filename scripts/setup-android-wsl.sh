#!/usr/bin/env bash
# ============================================================
#  Android build toolchain for WSL2 / Debian 13 (trixie)
#
#  WHY THIS ISN'T 100% APT
#  -----------------------
#  Debian's own Android/Gradle packages are years out of date and cannot
#  build a modern app:
#    * apt `gradle`                  = 4.4.1  (AGP 8.x needs 8.7+)
#    * apt `android-sdk-build-tools` = 29.0.3 (Android 10 era; no modern d8/R8)
#    * there is no apt package for platforms;android-34
#  So: the JDK comes from apt, and only the Android SDK + Gradle (which
#  Debian doesn't ship in a usable version) are fetched directly.
#
#  Downloads: ~400 MB
#
#  NOTE: this script does NOT touch ~/.bashrc. Run setup-shell-env.sh
#  afterwards to bake the environment permanently.
#
#  Usage:  bash setup-android-wsl.sh
# ============================================================
set -euo pipefail

SDK="$HOME/Android/Sdk"
TOOLS="$HOME/android-tools"
GRADLE_VER="8.7"

mkdir -p "$TOOLS"

echo "== 1/4  JDK + helpers from apt =="
sudo apt update
sudo apt install -y openjdk-21-jdk unzip curl
export JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
echo "   JAVA_HOME=$JAVA_HOME"
"$JAVA_HOME/bin/java" -version

echo "== 2/4  Android command-line tools =="
if [ ! -d "$SDK/cmdline-tools/latest" ]; then
  mkdir -p "$SDK/cmdline-tools"
  curl -L -o "$TOOLS/cmdline.zip" \
    "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"
  unzip -q "$TOOLS/cmdline.zip" -d "$SDK/cmdline-tools"
  mv "$SDK/cmdline-tools/cmdline-tools" "$SDK/cmdline-tools/latest"
  rm -f "$TOOLS/cmdline.zip"
fi
export ANDROID_HOME="$SDK"
export PATH="$JAVA_HOME/bin:$SDK/cmdline-tools/latest/bin:$SDK/platform-tools:$PATH"

echo "== 3/4  licences + only the SDK bits this project needs =="
yes 2>/dev/null | sdkmanager --sdk_root="$SDK" --licenses >/dev/null || true
sdkmanager --sdk_root="$SDK" "platform-tools" "platforms;android-34" "build-tools;34.0.0"

echo "== 4/4  Gradle $GRADLE_VER (apt ships 4.4.1, too old for AGP 8.x) =="
if [ ! -d "$TOOLS/gradle-$GRADLE_VER" ]; then
  curl -L -o "$TOOLS/gradle.zip" \
    "https://services.gradle.org/distributions/gradle-$GRADLE_VER-bin.zip"
  unzip -q "$TOOLS/gradle.zip" -d "$TOOLS"
  rm -f "$TOOLS/gradle.zip"
fi

echo
echo "=================== DONE ==================="
echo "Toolchain installed under $TOOLS and $SDK"
echo
echo "Next - bake the environment into ~/.bashrc:"
echo "  bash setup-shell-env.sh"
echo
echo "Then open a NEW shell and build:"
echo "  cd android"
echo "  echo \"sdk.dir=\$ANDROID_HOME\" > local.properties"
echo "  gradle assembleDebug"
