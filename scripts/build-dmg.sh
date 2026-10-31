#!/usr/bin/env bash
# Build a distributable MacEverything DMG for one CPU architecture.
#
# The Xcode project links re2/abseil from Homebrew, whose dylibs target the
# build machine's macOS and are not shipped in the app. For releases, both are
# built from source as static libraries for the target arch and deployment
# target, so the app runs on any Mac without Homebrew.
#
# Usage: scripts/build-dmg.sh <arm64|x86_64> [version]
# Output: dist/MacEverything-<version>-<arch>.dmg
set -euo pipefail

ARCH="${1:?usage: $0 <arm64|x86_64> [version]}"
VERSION="${2:-}"
case "$ARCH" in arm64|x86_64) ;; *) echo "unsupported arch: $ARCH" >&2; exit 1 ;; esac

ABSL_VERSION="20260817.0"
RE2_VERSION="2025-11-05"
DEPLOYMENT_TARGET="14.0"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build/release-$ARCH"
DEPS="$WORK/deps"
DIST="$ROOT/dist"
JOBS="$(sysctl -n hw.ncpu)"

if [ -z "$VERSION" ]; then
    VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$ROOT/MacEverything/Info.plist")"
fi

cmake_build() {
    local src="$1"; shift
    cmake -S "$src" -B "$src/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DCMAKE_PREFIX_PATH="$DEPS" \
        "$@"
    cmake --build "$src/build" --parallel "$JOBS"
    cmake --install "$src/build"
}

build_deps() {
    if [ -f "$DEPS/lib/libre2.a" ]; then
        echo "==> Reusing static deps in $DEPS"
        return
    fi
    local src="$WORK/src"
    rm -rf "$src" "$DEPS"
    mkdir -p "$src"
    echo "==> Building abseil $ABSL_VERSION ($ARCH)"
    curl -fsSL "https://github.com/abseil/abseil-cpp/archive/refs/tags/$ABSL_VERSION.tar.gz" | tar -xz -C "$src"
    # abseil's ABI depends on the C++ standard, so it must match the app's C++20.
    cmake_build "$src/abseil-cpp-$ABSL_VERSION" \
        -DABSL_PROPAGATE_CXX_STD=ON -DABSL_BUILD_TESTING=OFF -DABSL_ENABLE_INSTALL=ON
    echo "==> Building re2 $RE2_VERSION ($ARCH)"
    curl -fsSL "https://github.com/google/re2/archive/refs/tags/$RE2_VERSION.tar.gz" | tar -xz -C "$src"
    cmake_build "$src/re2-$RE2_VERSION" -DRE2_BUILD_TESTING=OFF
}

build_app() {
    echo "==> Building MacEverything $VERSION ($ARCH)"
    # ld64 resolves archives regardless of order, so every abseil archive can be
    # listed and only the objects re2 needs are pulled in.
    local ldflags="$DEPS/lib/libre2.a $(echo "$DEPS"/lib/libabsl_*.a)"
    rm -rf "$WORK/build"
    xcodebuild -project "$ROOT/MacEverything.xcodeproj" -scheme MacEverything \
        -configuration Release \
        -derivedDataPath "$WORK/DerivedData" \
        SYMROOT="$WORK/build" \
        ARCHS="$ARCH" ONLY_ACTIVE_ARCH=NO \
        MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        HEADER_SEARCH_PATHS="\$(SRCROOT)/MacEverything/Core \$(SRCROOT)/MacEverything/Bridge $DEPS/include" \
        LIBRARY_SEARCH_PATHS="" \
        OTHER_LDFLAGS="$ldflags" \
        CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY=- DEVELOPMENT_TEAM="" \
        build
}

verify_app() {
    local app="$1" bin
    for bin in "$app/Contents/MacOS/"*; do
        if [ "$(lipo -archs "$bin")" != "$ARCH" ]; then
            echo "error: $bin is $(lipo -archs "$bin"), expected $ARCH" >&2; exit 1
        fi
        if otool -L "$bin" | grep -qE '/opt/homebrew|/usr/local'; then
            echo "error: $bin links a non-system library:" >&2; otool -L "$bin" >&2; exit 1
        fi
    done
}

package() {
    local app="$WORK/build/Release/MacEverything.app"
    verify_app "$app"
    # Info.plist hardcodes the version, so stamp the release version and re-sign.
    /usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$app/Contents/Info.plist"
    codesign --force --deep --sign - "$app"
    codesign --verify --deep --strict "$app"

    local stage="$WORK/dmg" dmg="$DIST/MacEverything-$VERSION-$ARCH.dmg"
    rm -rf "$stage"
    mkdir -p "$stage" "$DIST"
    cp -R "$app" "$stage/"
    ln -s /Applications "$stage/Applications"
    hdiutil create -volname "MacEverything" -srcfolder "$stage" -ov -format UDZO "$dmg"
    echo "==> $dmg"
}

build_deps
build_app
package
