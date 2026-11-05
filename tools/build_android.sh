#!/usr/bin/env bash
# Build the Android APK without Gradle, on Linux:
#   - SDL3 (shared) and the port (libmain.so) with the NDK, per ABI
#   - SDL's Java glue + android/java with javac and d8
#   - android/res, the manifest and the bundled font with aapt2
#   - then zipalign and apksigner
#
#   tools/build_android.sh SDL3_SOURCE_DIR [OUT_DIR]
#
# Needs: build/gen (tools/emit.py), third_party (see README), the Android SDK
# in $ANDROID_HOME (default ~/android-sdk) with an NDK, build-tools 36 and the
# android-36 platform, a JDK (17+), CMake, Ninja, python3.
# Signing: $KEYSTORE (default ~/.psiv-keys/psiv-alpha.jks, made on first use
# with a random password beside it, .pass). Keep that key: Android installs
# an update only over an app signed with the same one.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDL_SRC=$(cd "$1" && pwd)
OUT=${2:-$ROOT/build/android}
SDK=${ANDROID_HOME:-$HOME/android-sdk}
NDK=${ANDROID_NDK:-$(ls -d "$SDK"/ndk/* | sort -V | tail -1)}
BT=$SDK/build-tools/${BUILD_TOOLS:-36.0.0}
JAR=$SDK/platforms/${ANDROID_PLATFORM_DIR:-android-36}/android.jar
MIN_API=24
TARGET_API=35
ABIS=${ABIS:-arm64-v8a}
VERSION=${PSIV_VERSION:-0.1.0-alpha+$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo local)}
VERSION_CODE=${VERSION_CODE:-$(date -u +%y%m%d%H)}
KEYSTORE=${KEYSTORE:-$HOME/.psiv-keys/psiv-alpha.jks}
PASS=${KEYSTORE%.jks}.pass
TOOLCHAIN=$NDK/build/cmake/android.toolchain.cmake
mkdir -p "$OUT"
echo "PSIV Recomp $VERSION ($VERSION_CODE) for $ABIS, NDK $(basename "$NDK")"

# 1. native code, per ABI
rm -rf "$OUT/apk"
for abi in $ABIS; do
  echo "== $abi: SDL3"
  cmake -S "$SDL_SRC" -B "$OUT/sdl3-$abi" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$MIN_API" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_ANDROID_JAR=OFF -DSDL_INSTALL_DOCS=OFF -DCMAKE_INSTALL_PREFIX="$OUT/sdl3-$abi-install" > "$OUT/sdl3-$abi.log"
  ninja -C "$OUT/sdl3-$abi" install >> "$OUT/sdl3-$abi.log"
  echo "== $abi: the port"
  cmake -S "$ROOT" -B "$OUT/psiv-$abi" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$MIN_API" \
    -DANDROID_STL=c++_static -DSDL3_DIR="$OUT/sdl3-$abi-install/lib/cmake/SDL3" \
    -DCMAKE_FIND_ROOT_PATH="$OUT/sdl3-$abi-install" -DPSIV_VERSION="$VERSION" > "$OUT/psiv-$abi.log"
  ninja -C "$OUT/psiv-$abi" psiv >> "$OUT/psiv-$abi.log" || { grep -E "error|FAILED" "$OUT/psiv-$abi.log" | head -30; exit 1; }
  mkdir -p "$OUT/apk/lib/$abi"
  cp "$OUT/sdl3-$abi-install/lib/libSDL3.so" "$OUT/psiv-$abi/libmain.so" "$OUT/apk/lib/$abi/"
  "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$OUT/apk/lib/$abi/"*.so
done

# 2. resources, the manifest and the font -> base.apk (+ R.java)
echo "== resources"
J=$OUT/java
rm -rf "$J" "$OUT/assets"
mkdir -p "$J/gen" "$J/classes" "$OUT/assets/fonts"
cp "$ROOT/res/fonts/"* "$OUT/assets/fonts/"
"$BT/aapt2" compile --dir "$ROOT/android/res" -o "$J/res.zip"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$JAR" --manifest "$ROOT/android/AndroidManifest.xml" \
  --min-sdk-version $MIN_API --target-sdk-version $TARGET_API \
  --version-code "$VERSION_CODE" --version-name "$VERSION" \
  -A "$OUT/assets" --java "$J/gen" "$J/res.zip"

# 3. Java: SDL's glue, our activity, R -> classes.dex
echo "== java"
javac -encoding UTF-8 -source 8 -target 8 -bootclasspath "$JAR" -classpath "$JAR" -d "$J/classes" -nowarn -Xlint:-options \
  $(find "$SDL_SRC/android-project/app/src/main/java" "$ROOT/android/java" "$J/gen" -name '*.java')
(cd "$J/classes" && jar cf "$J/classes.jar" .)
"$BT/d8" --release --min-api $MIN_API --lib "$JAR" --output "$J" "$J/classes.jar"

# 4. the APK: add the dex and the libraries, align, sign
echo "== apk"
cp "$OUT/base.apk" "$OUT/unsigned.apk"
python3 -I - "$OUT/unsigned.apk" "$J/classes.dex" "$OUT/apk" <<'EOF'
import os, sys, zipfile
apk, dex, root = sys.argv[1:4]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(dex, 'classes.dex')
    for d, _, files in sorted(os.walk(os.path.join(root, 'lib'))):
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root))
EOF
"$BT/zipalign" -f -P 16 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
if [ ! -f "$KEYSTORE" ]; then
  echo "== a new signing key: $KEYSTORE"
  mkdir -p "$(dirname "$KEYSTORE")"
  chmod 700 "$(dirname "$KEYSTORE")"
  (umask 077 && head -c 24 /dev/urandom | base64 | tr -d '/+=\n' > "$PASS")
  keytool -genkeypair -keystore "$KEYSTORE" -storetype PKCS12 -storepass:file "$PASS" -keypass:file "$PASS" \
    -alias psiv -keyalg RSA -keysize 4096 -validity 10000 -dname "CN=PSIV Recomp, O=PsiLok" -noprompt
fi
APK=$OUT/psiv-recomp-$VERSION_CODE.apk
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass "file:$PASS" --ks-key-alias psiv --out "$APK" "$OUT/aligned.apk"
"$BT/apksigner" verify --verbose "$APK" | grep -E "Verified using|Number of signers" || true
"$BT/aapt2" dump badging "$APK" | grep -E "^package:|^sdkVersion|^targetSdkVersion|^native-code|^application-label:"
ls -la "$APK"
