#!/usr/bin/env bash
# Submits a zip or DMG to Apple's notary service and waits for the verdict. On
# anything but "Accepted" it prints the notary log and fails.
#
# Usage: NOTARY_APPLE_ID=… NOTARY_TEAM_ID=… NOTARY_APP_PASSWORD=… scripts/release/notarize.sh file
set -euo pipefail

FILE="${1:?usage: $0 file.zip|file.dmg}"
auth=(--apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" --password "$NOTARY_APP_PASSWORD")

result="$(xcrun notarytool submit "$FILE" "${auth[@]}" --wait --timeout 30m --output-format json)"
echo "$result"
field() { python3 -c 'import json, sys; print(json.load(sys.stdin).get(sys.argv[1], ""))' "$1" <<<"$result"; }
id="$(field id)"
status="$(field status)"
if [[ "$status" != Accepted ]]; then
  xcrun notarytool log "$id" "${auth[@]}" || true
  echo "::error::notarization of $(basename "$FILE") ended with status '$status'"
  exit 1
fi
