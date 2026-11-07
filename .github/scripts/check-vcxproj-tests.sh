#!/bin/bash
# Fail if a tests/**/*.cc is not listed as a ClCompile in any vsprojects/**/*.vcxproj.
# The Makefile globs tests/, so Linux and macOS pick new files up; MSVC only builds what a vcxproj names.
# Each Include is resolved against its own project's directory before the comparison.

set -u
cd "$(dirname "$0")/../.." || exit 2
root=$(pwd -P)

projects=()
while IFS= read -r -d '' p; do projects+=("$p"); done < <(find vsprojects -name '*.vcxproj' -print0)
if [ "${#projects[@]}" -eq 0 ]; then
    echo "no vcxproj found under vsprojects/" >&2
    exit 2
fi

listed=$(mktemp)
trap 'rm -f "$listed"' EXIT
for p in "${projects[@]}"; do
    dir=$(dirname "$p")
    while IFS= read -r inc; do
        inc=${inc//\\//}
        realpath -m --relative-to="$root" "$dir/$inc"
    done < <(grep -o '<ClCompile Include="[^"]*"' "$p" | sed 's/^<ClCompile Include="//; s/"$//') >> "$listed"
done

found=$(mktemp)
trap 'rm -f "$listed" "$found"' EXIT
if ! find tests -name '*.cc' -print0 > "$found"; then
    echo "find tests failed" >&2
    exit 2
fi
tests=()
while IFS= read -r -d '' t; do tests+=("$t"); done < "$found"
if [ "${#tests[@]}" -eq 0 ]; then
    echo "found no tests/**/*.cc, the check is blind" >&2
    exit 2
fi

status=0
for f in "${tests[@]}"; do
    if ! grep -qxF "$f" "$listed"; then
        echo "missing from every vcxproj: $f"
        status=1
    fi
done
echo "checked ${#tests[@]} test files against ${#projects[@]} projects"
exit $status
