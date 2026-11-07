#!/bin/bash
# Fail if a tests/**/*.cc is not listed as a ClCompile in any vsprojects/**/*.vcxproj.
# The Makefile globs tests/, so Linux and macOS pick new files up; MSVC only builds what a vcxproj names.

set -u
cd "$(dirname "$0")/../.." || exit 2

projects=$(find vsprojects -name '*.vcxproj')
if [ -z "$projects" ]; then
    echo "no vcxproj found under vsprojects/" >&2
    exit 2
fi

status=0
count=0
while IFS= read -r f; do
    count=$((count + 1))
    win=${f//\//\\}
    # shellcheck disable=SC2086
    if ! grep -qF "<ClCompile Include=\"..\\..\\..\\${win}\"" $projects; then
        echo "missing from every vcxproj: $f"
        status=1
    fi
done < <(find tests -name '*.cc' | sort)

if [ "$count" -eq 0 ]; then
    echo "found no tests/**/*.cc, the check is blind" >&2
    exit 2
fi
echo "checked $count test files"
exit $status
