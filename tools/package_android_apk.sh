#!/usr/bin/env bash
# Packages the Ksila APK without Gradle: aapt2 + zip + zipalign + apksigner.
#
# The app has no Java code (android:hasCode="false", NativeActivity), so no
# dex step is needed — just resources, the manifest and libksila.so.
#
# Usage:
#   package_android_apk.sh --out Ksila-lobby.apk PATH/libksila.so=arm64-v8a [more...]
#
# Requires: ANDROID_HOME with build-tools + platforms installed, and a JDK
# (for apksigner). Signs with the debug key from android/signing/.
set -euo pipefail

SDK="${ANDROID_HOME:?ANDROID_HOME must point to the Android SDK}"
OUT=""
LIBS=()

while [ $# -gt 0 ]; do
	case "$1" in
		--out) OUT="$2"; shift 2 ;;
		--) shift; break ;;
		*) LIBS+=("$1"); shift ;;
	esac
done

if [ -z "$OUT" ] || [ "${#LIBS[@]}" -eq 0 ]; then
	echo "usage: $0 --out X.apk libksila.so=abi [libksila.so=abi ...]" >&2
	exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MANIFEST="$ROOT/android/AndroidManifest.xml"
RES_DIR="$ROOT/android/res"
KEY="$ROOT/android/signing/key.pk8"
CERT="$ROOT/android/signing/cert.x509.pem"

BT="$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1)"
PLATFORM_JAR="$(ls -d "$SDK"/platforms/android-* 2>/dev/null | sort -V | tail -1)/android.jar"
[ -d "$BT" ] || { echo "no build-tools under $SDK" >&2; exit 1; }
[ -f "$PLATFORM_JAR" ] || { echo "no platform android.jar under $SDK" >&2; exit 1; }
echo "build-tools: $BT"
echo "platform:   $PLATFORM_JAR"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/stage"
mkdir -p "$STAGE/lib"

# 1) Stage native libraries as lib/<abi>/libksila.so.
for pair in "${LIBS[@]}"; do
	so="${pair%%=*}"
	abi="${pair##*=}"
	[ -f "$so" ] || { echo "missing: $so" >&2; exit 1; }
	mkdir -p "$STAGE/lib/$abi"
	cp "$so" "$STAGE/lib/$abi/libksila.so"
	echo "staged $abi <- $so"
done

# 2) Compile resources and link the manifest into a base APK.
"$BT/aapt2" compile --dir "$RES_DIR" -o "$WORK/res.zip"
"$BT/aapt2" link -o "$WORK/base.apk" -I "$PLATFORM_JAR" --manifest "$MANIFEST" "$WORK/res.zip"
echo "linked base.apk"

# 3) Add the native libraries.
if ! command -v zip >/dev/null; then
	echo "'zip' is required" >&2
	exit 1
fi
(cd "$STAGE" && zip -q "$WORK/base.apk" -r lib)

# 4) Align.
"$BT/zipalign" -f 4 "$WORK/base.apk" "$WORK/aligned.apk"

# 5) Sign (v1+v2).
mkdir -p "$(dirname "$OUT")"
"$BT/apksigner" sign --key "$KEY" --cert "$CERT" --out "$OUT" "$WORK/aligned.apk"
"$BT/apksigner" verify --print-certs "$OUT" || true

echo "APK ready: $OUT ($(du -h "$OUT" | cut -f1))"
