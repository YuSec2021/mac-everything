# Release process

`.github/workflows/release.yml` builds MacEverything on a GitHub-hosted macOS
runner, packages it, and publishes a GitHub Release.

## Triggers

- **Push a tag** `v<version>`, e.g. `git tag v1.2.0 && git push origin v1.2.0`.
  A tag with a suffix (`v1.3.0-beta.1`) is published as a pre-release.
- **Run manually** (Actions → Release → Run workflow, or
  `gh workflow run release.yml -f version=1.2.1 -f publish=false`).
  `publish=false` (the default) only builds and attaches the files to the run as
  a workflow artifact; `publish=true` also creates the Release and the tag
  `v<version>` on the selected commit.

## What the workflow does

1. `brew install re2 abseil`, then `make test-fast` (fast unit tests plus the
   bridge lint). Part 4, the FSEvents integration test, is part of `--slow` and
   is **not run**: hosted runners may not deliver FSEvents, and that should not
   block a release. Run `make test-slow` locally before tagging.
2. Writes the version into `MacEverything/Info.plist` (in the runner's checkout
   only; nothing is committed):
   - `CFBundleShortVersionString` = the tag without `v` and without any
     `-suffix` (`v1.2.0-beta.1` → `1.2.0`), since that key allows digits and
     dots only.
   - `CFBundleVersion` = `git rev-list --count HEAD`, the number of commits
     reachable from the built commit. It increases with every commit on `main`
     and is reproducible from the tag alone.
3. `make app` with `ARCHS=arm64` and an ad-hoc identity (the real signing
   happens later).
4. `scripts/release/bundle_dylibs.sh` copies `libre2` and its whole abseil
   closure into `Contents/Frameworks` and rewrites install names to
   `@rpath/…` / `@loader_path/…`, so the app runs on a Mac without Homebrew.
5. `scripts/release/sign_app.sh` signs the dylibs, then the app (see below).
6. Optional notarization and stapling of the app.
7. `scripts/release/verify_app.sh` fails the release if any load path still
   points into Homebrew, `codesign --verify --deep --strict` fails,
   `LSEnvironment` → `MallocLargeCache` is not `0`, the version is wrong, or
   the app does not stay running for 6 seconds. For ad-hoc builds it also
   launches with `DYLD_PRINT_LIBRARIES` and fails if dyld loads anything from
   Homebrew.
8. Packages `MacEverything-<version>.zip` and `MacEverything-<version>.dmg`
   (`make dmg-package`), signs/notarizes/staples the DMG when credentials
   exist, writes `SHA256SUMS.txt`, uploads everything as a workflow artifact,
   and (when publishing) creates the Release with those files.

### Why bundle the dylibs instead of linking statically

Homebrew's abseil ships only dylibs, and `libre2.a` still needs abseil, so a
static build would mean compiling re2 and abseil from source in CI. Bundling the
Homebrew bottles reuses the exact libraries `make test-fast` just tested and
keeps `make app` unchanged. The cost is about 4–5 MB in `Contents/Frameworks`
(about 66 small abseil dylibs).

### Minimum macOS

Homebrew bottles are built for the macOS they target, and dyld refuses a
library whose minimum OS is newer than the running system. `bundle_dylibs.sh`
therefore raises `LSMinimumSystemVersion` to the highest minimum OS among the
bundled libraries, and the release notes state it. With the `macos-15` runner
that is macOS 15, even though the Xcode deployment target is 14.0. Supporting
macOS 14 would mean building re2/abseil from source with
`MACOSX_DEPLOYMENT_TARGET=14.0` (or running on a macOS 14 runner).

## Signing and notarization secrets

Set these under Settings → Secrets and variables → Actions. Never commit any of
them.

| Secret | Required for | Value |
| --- | --- | --- |
| `MACOS_CERTIFICATE_P12_BASE64` | Developer ID signing | `base64 -i DeveloperID.p12` of a *Developer ID Application* certificate with its private key |
| `MACOS_CERTIFICATE_PASSWORD` | Developer ID signing | Password of that `.p12` |
| `MACOS_SIGNING_IDENTITY` | optional | Full identity name, e.g. `Developer ID Application: Name (TEAMID)`; defaults to the first Developer ID Application identity in the `.p12` |
| `NOTARY_APPLE_ID` | notarization | Apple ID email of a team member |
| `NOTARY_TEAM_ID` | notarization | 10-character Team ID |
| `NOTARY_APP_PASSWORD` | notarization | App-specific password from appleid.apple.com |

Behavior:

- **Certificate and notary secrets set**: hardened-runtime Developer ID
  signature with a secure timestamp; the app and the DMG are notarized and
  stapled.
- **Certificate only**: Developer ID signature, not notarized; the release notes
  say so.
- **Neither**: ad-hoc signature, not notarized; the release notes say so and
  tell users to right-click → Open or remove the quarantine attribute.

## Checking a downloaded build

```bash
otool -L MacEverything.app/Contents/MacOS/MacEverything | grep homebrew   # expect no output
codesign --verify --deep --strict MacEverything.app && echo signed-ok
/usr/libexec/PlistBuddy -c 'Print :LSEnvironment' MacEverything.app/Contents/Info.plist
```

`scripts/release/verify_app.sh path/to/MacEverything.app` runs all of these;
set `VERIFY_SKIP_LAUNCH=1` when another copy of the app is already running.
