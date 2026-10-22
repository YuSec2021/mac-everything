#!/usr/bin/env bash
# Copies every Homebrew dylib the app links (re2 + its abseil closure) into
# Contents/Frameworks and rewrites install names so the app runs on a Mac
# without Homebrew. Also raises LSMinimumSystemVersion to the highest minos of
# the bundled libraries: Homebrew bottles target the OS they were built on, and
# dyld refuses to load them on anything older.
#
# Usage: scripts/release/bundle_dylibs.sh path/to/MacEverything.app
set -euo pipefail

APP="${1:?usage: $0 path/to/App.app}"
BREW_PREFIX="${HOMEBREW_PREFIX:-$(brew --prefix)}"
EXE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist")"
EXE="$APP/Contents/MacOS/$EXE_NAME"
FW="$APP/Contents/Frameworks"
mkdir -p "$FW"

# Prints the non-system dependencies (Homebrew paths, @rpath/@loader_path) of a Mach-O.
deps_of() {
  otool -L "$1" | tail -n +2 | awk '{print $1}' \
    | grep -E "^($BREW_PREFIX/|@rpath/|@loader_path/)" || true
}

# Resolves a dependency reference to a real file under the Homebrew prefix.
resolve() {
  local ref="$1" from="$2" base
  case "$ref" in
    "$BREW_PREFIX"/*) realpath "$ref" ;;
    @loader_path/*) realpath "$(dirname "$(realpath "$from")")/${ref#@loader_path/}" ;;
    @rpath/*)
      base="${ref#@rpath/}"
      # Bundled copies and the executable carry no Homebrew rpaths; look next to the
      # referencing library first, then in the Homebrew lib directory.
      for dir in "$(dirname "$(realpath "$from")")" "$BREW_PREFIX/lib"; do
        [[ -e "$dir/$base" ]] && { realpath "$dir/$base"; return; }
      done
      echo "cannot resolve $ref from $from" >&2; return 1 ;;
  esac
}

# Breadth-first copy of the dependency closure. Each library is bundled under the
# basename the linker recorded (e.g. libre2.11.dylib), not the versioned real file.
# Written for the stock bash 3.2: no associative arrays, so the Homebrew source of
# each bundled copy is kept as "name<TAB>path" lines for resolving its own refs.
queue=("$EXE")
sources=""
i=0
while ((i < ${#queue[@]})); do
  cur="${queue[$i]}"; i=$((i + 1))
  cur_src="$(printf '%s' "$sources" | awk -F'\t' -v n="$(basename "$cur")" '$1 == n {print $2; exit}')"
  while IFS= read -r ref; do
    [[ -z "$ref" ]] && continue
    name="$(basename "$ref")"
    if ! printf '%s' "$sources" | awk -F'\t' -v n="$name" '$1 == n {f=1} END {exit !f}'; then
      src="$(resolve "$ref" "${cur_src:-$cur}")"
      cp -f "$src" "$FW/$name"
      chmod u+w "$FW/$name"
      install_name_tool -id "@rpath/$name" "$FW/$name" 2>/dev/null
      sources+="$name"$'\t'"$src"$'\n'
      queue+=("$FW/$name")
      echo "bundled $name <- $src"
    fi
    if [[ "$cur" == "$EXE" ]]; then
      new="@rpath/$name"  # the app target already has @executable_path/../Frameworks
    else
      new="@loader_path/$name"
    fi
    if [[ "$ref" != "$new" ]]; then install_name_tool -change "$ref" "$new" "$cur" 2>/dev/null; fi
  done < <(deps_of "$cur")
done

# Fail loudly if anything still points into Homebrew.
leftover="$(for f in "$EXE" "$FW"/*.dylib; do otool -L "$f" | tail -n +2; done | grep -F "$BREW_PREFIX" || true)"
if [[ -n "$leftover" ]]; then
  echo "error: Homebrew references remain:" >&2; echo "$leftover" >&2; exit 1
fi

# Raise LSMinimumSystemVersion if a bundled library needs a newer macOS.
plist="$APP/Contents/Info.plist"
min_os="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$plist")"
for f in "$FW"/*.dylib; do
  lib_min="$(vtool -show-build "$f" | awk '/minos/ {print $2; exit}')"
  [[ -z "$lib_min" ]] && continue
  if [[ "$(printf '%s\n%s\n' "$min_os" "$lib_min" | sort -V | tail -1)" != "$min_os" ]]; then
    echo "$(basename "$f") requires macOS $lib_min; raising LSMinimumSystemVersion from $min_os"
    min_os="$lib_min"
  fi
done
/usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $min_os" "$plist"
echo "LSMinimumSystemVersion=$min_os"
echo "bundled $(ls "$FW"/*.dylib | wc -l | tr -d ' ') dylibs into $FW"
