#!/usr/bin/env bash
# Build the AndroidAssetBackend test APK without Gradle: NDK (CMake) + aapt2 + zipalign + apksigner.
# Env: ANDROID_NDK, ANDROID_SDK (with build-tools;35.0.0 and platforms;android-35), optional ABI (default arm64-v8a).
# Output: build-apk/vfs-asset-test.apk
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$HERE/../.."
ABI="${ABI:-arm64-v8a}"
BT="$ANDROID_SDK/build-tools/35.0.0"
JAR="$ANDROID_SDK/platforms/android-35/android.jar"
OUT="$REPO/build-apk"
rm -rf "$OUT/stage" && mkdir -p "$OUT/stage/assets" "$OUT/stage/lib/$ABI"

# 1. Host vfstool (to pack game.vfs).
cmake -S "$REPO" -B "$OUT/host" -DCMAKE_BUILD_TYPE=Release -DVFS_BUILD_TESTS=OFF > /dev/null
cmake --build "$OUT/host" --target vfstool -j > /dev/null

# 2. Assets.
python3 "$HERE/gen_assets.py" "$OUT/stage/assets" "$OUT/arcsrc"
"$OUT/host/tools/vfstool/vfstool" pack "$OUT/arcsrc" -o "$OUT/stage/assets/game.vfs" --compress "*.lua" > /dev/null

# 3. Native library.
cmake -S "$HERE" -B "$OUT/ndk" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$OUT/ndk" -j > /dev/null
cp "$OUT/ndk/libvfsassettest.so" "$OUT/stage/lib/$ABI/"

# 4. Package: .vfs and .bin stay uncompressed (fd / pread path); .txt is compressed. Native libs stored uncompressed.
"$BT/aapt2" link -o "$OUT/unsigned.apk" -I "$JAR" --manifest "$HERE/AndroidManifest.xml" -A "$OUT/stage/assets" -0 vfs -0 bin
python3 "$HERE/add_lib.py" "$OUT/unsigned.apk" "$OUT/withlib.apk" "$OUT/stage/lib/$ABI/libvfsassettest.so" "lib/$ABI/libvfsassettest.so"
"$BT/zipalign" -f -P 16 4 "$OUT/withlib.apk" "$OUT/aligned.apk"
KS="$OUT/debug.keystore"
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" > /dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --out "$OUT/vfs-asset-test.apk" "$OUT/aligned.apk"
echo "$OUT/vfs-asset-test.apk"
