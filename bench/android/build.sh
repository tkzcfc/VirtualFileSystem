#!/usr/bin/env bash
# Build the vfs_bench APK without Gradle: NDK (CMake) + aapt2 + zipalign + apksigner.
# Env: ANDROID_NDK, ANDROID_SDK (with build-tools;35.0.0 and platforms;android-35), optional ABI (default arm64-v8a),
#      optional BENCH_SCALE (dataset scale, default 1) and STORE_DATA=1 (store the loose dataset files uncompressed;
#      by default aapt2 compresses them, as Gradle does for ordinary assets). plain.vfs is always stored uncompressed.
# Output: build-bench-apk/vfs-bench.apk
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$HERE/../.."
ABI="${ABI:-arm64-v8a}"
SCALE="${BENCH_SCALE:-1}"
BT="$ANDROID_SDK/build-tools/35.0.0"
JAR="$ANDROID_SDK/platforms/android-35/android.jar"
OUT="$REPO/build-bench-apk"
rm -rf "$OUT/stage" "$OUT/data" && mkdir -p "$OUT/stage/assets/bench" "$OUT/stage/lib/$ABI"

# 1. Host vfstool (to pack plain.vfs).
cmake -S "$REPO" -B "$OUT/host" -DCMAKE_BUILD_TYPE=Release -DVFS_BUILD_TESTS=OFF > /dev/null
cmake --build "$OUT/host" --target vfstool -j > /dev/null

# 2. Dataset: the same files vfs_bench generates, plus assets.lst; plain.vfs packs them (without the manifest).
python3 "$REPO/tests/android-apk/gen_assets.py" --bench "$OUT/data" "$SCALE"
mv "$OUT/data/assets.lst" "$OUT/assets.lst"
"$OUT/host/tools/vfstool/vfstool" pack "$OUT/data" -o "$OUT/stage/assets/bench/plain.vfs" > /dev/null
mv "$OUT/data" "$OUT/stage/assets/bench/data"
mv "$OUT/assets.lst" "$OUT/stage/assets/bench/data/assets.lst"

# 3. Native library.
cmake -S "$HERE" -B "$OUT/ndk" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$OUT/ndk" -j > /dev/null
cp "$OUT/ndk/libvfsbench.so" "$OUT/stage/lib/$ABI/"

# 4. Package. Native libs are stored uncompressed (extractNativeLibs=false).
NOCOMP=(-0 vfs)
[ "${STORE_DATA:-0}" = 1 ] && NOCOMP+=(-0 bin)
"$BT/aapt2" link -o "$OUT/unsigned.apk" -I "$JAR" --manifest "$HERE/AndroidManifest.xml" -A "$OUT/stage/assets" "${NOCOMP[@]}"
python3 "$REPO/tests/android-apk/add_lib.py" "$OUT/unsigned.apk" "$OUT/withlib.apk" "$OUT/stage/lib/$ABI/libvfsbench.so" "lib/$ABI/libvfsbench.so"
"$BT/zipalign" -f -P 16 4 "$OUT/withlib.apk" "$OUT/aligned.apk"
KS="$OUT/debug.keystore"
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" > /dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --out "$OUT/vfs-bench.apk" "$OUT/aligned.apk"
echo "$OUT/vfs-bench.apk"
