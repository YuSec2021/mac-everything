#!/usr/bin/env bash
# Release gate for a packaged app: no Homebrew load paths, a valid signature,
# LSEnvironment (MallocLargeCache=0) intact, the expected version, and a process
# that stays up for a few seconds with every dylib loaded from inside the bundle.
#
# Usage: scripts/release/verify_app.sh App.app [expected-short-version]
# Set VERIFY_SKIP_LAUNCH=1 to skip the launch check (e.g. locally, while another
# copy of the app is already running and owns the index).
set -euo pipefail

APP="${1:?usage: $0 path/to/App.app [version]}"
EXPECTED_VERSION="${2:-}"
PLIST="$APP/Contents/Info.plist"
EXE="$APP/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$PLIST")"
fail=0
check() { if [[ "$1" == ok ]]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }

homebrew_refs="$(for f in "$EXE" "$APP"/Contents/Frameworks/*.dylib; do
  [[ -e "$f" ]] && otool -L "$f" | tail -n +2; done | grep -E '/opt/homebrew|/usr/local/(opt|Cellar)' || true)"
[[ -z "$homebrew_refs" ]] && check ok "no Homebrew load paths (otool -L)" || { check fail "Homebrew load paths:"; echo "$homebrew_refs"; }

codesign --verify --deep --strict "$APP" && check ok "codesign --verify --deep --strict" || check fail "codesign --verify"

malloc="$(/usr/libexec/PlistBuddy -c 'Print :LSEnvironment:MallocLargeCache' "$PLIST" 2>/dev/null || true)"
[[ "$malloc" == "0" ]] && check ok "LSEnvironment MallocLargeCache=0" || check fail "LSEnvironment MallocLargeCache is '$malloc', expected 0"

version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$PLIST")"
build="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$PLIST")"
if [[ -z "$EXPECTED_VERSION" || "$version" == "$EXPECTED_VERSION" ]]; then
  check ok "version $version ($build)"
else
  check fail "CFBundleShortVersionString is $version, expected $EXPECTED_VERSION"
fi
echo "     LSMinimumSystemVersion $(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$PLIST")"

# Launch smoke test. DYLD_PRINT_LIBRARIES is honored for ad-hoc builds (no hardened
# runtime); under the hardened runtime dyld ignores it and only liveness is checked.
if [[ "${VERIFY_SKIP_LAUNCH:-}" == 1 ]]; then
  echo "skip launch check (VERIFY_SKIP_LAUNCH=1)"; exit $fail
fi
log="$(mktemp)"
DYLD_PRINT_LIBRARIES=1 "$EXE" >"$log" 2>&1 &
pid=$!
sleep 6
if kill -0 "$pid" 2>/dev/null; then
  check ok "app still running after 6 s"
  kill "$pid"; wait "$pid" 2>/dev/null || true
else
  wait "$pid" && rc=0 || rc=$?
  check fail "app exited early (status $rc):"; tail -20 "$log"
fi
if grep -qE '/opt/homebrew|/usr/local/(opt|Cellar)' "$log"; then
  check fail "dyld loaded a Homebrew library:"; grep -E '/opt/homebrew|/usr/local/(opt|Cellar)' "$log" | head
elif grep -q 'libre2' "$log"; then
  check ok "dyld loaded libre2 from $(grep -m1 'libre2' "$log" | sed 's/^.*dyld[^/]*//')"
fi
rm -f "$log"

exit $fail
