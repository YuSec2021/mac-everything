#!/usr/bin/env bash
# Signs the bundled dylibs, then the app. With SIGN_IDENTITY set (a Developer ID
# Application identity in the keychain) it signs with the hardened runtime and a
# secure timestamp, as notarization requires; otherwise it signs ad-hoc.
#
# Usage: SIGN_IDENTITY="Developer ID Application: …" scripts/release/sign_app.sh App.app entitlements.plist
set -euo pipefail

APP="${1:?usage: $0 path/to/App.app entitlements.plist}"
ENTITLEMENTS="${2:?usage: $0 path/to/App.app entitlements.plist}"

if [[ -n "${SIGN_IDENTITY:-}" ]]; then
  flags=(--force --sign "$SIGN_IDENTITY" --options runtime --timestamp)
else
  flags=(--force --sign - --timestamp=none)
fi

# Inside-out: nested code first, the bundle last.
for lib in "$APP"/Contents/Frameworks/*.dylib; do
  [[ -e "$lib" ]] || continue
  codesign "${flags[@]}" "$lib"
done
codesign "${flags[@]}" --entitlements "$ENTITLEMENTS" "$APP"

codesign --verify --deep --strict --verbose=1 "$APP"
codesign -dv "$APP" 2>&1 | grep -E '^(Identifier|Signature|Authority|TeamIdentifier|Runtime)' || true
