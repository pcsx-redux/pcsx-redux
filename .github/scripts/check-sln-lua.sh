#!/bin/bash
# Fail if the tests/lua/*.lua files and the ones the Visual Studio solution lists differ, in either direction.
# The solution only shows what it names, so a new script without a line is invisible in the IDE.

set -u
cd "$(dirname "$0")/../.." || exit 2

sln=vsprojects/pcsx-redux.sln
[ -f "$sln" ] || { echo "$sln not found" >&2; exit 2; }

files=$(mktemp)
listed=$(mktemp)
trap 'rm -f "$files" "$listed"' EXIT

if ! find tests/lua -name '*.lua' | sort > "$files"; then
    echo "find tests/lua failed" >&2
    exit 2
fi
if ! grep -o '\.\.\\tests\\lua\\[^ =]*\.lua' "$sln" | sed 's/^\.\.\\//; s/\\/\//g' | sort -u > "$listed"; then
    echo "found no tests/lua entries in $sln, the check is blind" >&2
    exit 2
fi
n=$(wc -l < "$files")
if [ "$n" -eq 0 ]; then
    echo "found no tests/lua/*.lua, the check is blind" >&2
    exit 2
fi

status=0
while IFS= read -r f; do
    echo "missing from $sln: $f"
    status=1
done < <(comm -23 "$files" "$listed")
while IFS= read -r f; do
    echo "listed in $sln but not on disk: $f"
    status=1
done < <(comm -13 "$files" "$listed")
echo "checked $n tests/lua files against $(wc -l < "$listed") solution entries"
exit $status
