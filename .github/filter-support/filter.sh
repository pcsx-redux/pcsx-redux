#!/bin/sh

set -e

ROOT=$(dirname $0)
CWD=$(pwd)
cd $ROOT
ROOT=$(pwd)
cd $CWD

# Delete non-tools source code
git filter-branch -f --tree-filter '(mv src/mips-common . || true) && (mv src/support . || true) && (mv src/supportpsx . || true) && rm -rf src && mkdir -p src && (mv mips-common src || true) && (mv support src || true) && mv supportpsx src || true' --tag-name-filter cat --prune-empty

# Delete some root files.
git filter-branch -f --tree-filter 'rm -f *.yml LICENSE* mips.ps1 TODO.md' --tag-name-filter cat --prune-empty

# Delete irrelevant folders
git filter-branch -f --tree-filter 'rm -rf .github hardware i18n resources .vscode vsprojects tests/pcsxrunner' --tag-name-filter cat --prune-empty

# Need to delete submodules actively: filter-branch does not see one that a
# tree filter deletes. The second pass also does src's (src/mips and
# src/pocketstation), which the first filter above leaves.
# Done in two passes for speed.
git filter-branch -f --tree-filter 'find third_party -maxdepth 1 -type d -and -not -name third_party -and -not -path third_party/cueparser* -and -not -path third_party/ELFIO\* -and -not -path third_party/expected\* -and -not -path third_party/fmt\* -and -not -path third_party/googletest\* -and -not -path third_party/iec-60908b\* -and -not -path third_party/magic_enum\* -and -not -path third_party/PEGTL\* -and -not -path third_party/ucl\* -and -not -path third_party/args\* -and -not -path third_party/stb\* -exec rm -rf {} \; || true' --tag-name-filter cat --prune-empty
git filter-branch -f --tree-filter 'find third_party -maxdepth 1 -type d -and -not -name third_party -and -not -path third_party/cueparser* -and -not -path third_party/ELFIO\* -and -not -path third_party/expected\* -and -not -path third_party/fmt\* -and -not -path third_party/googletest\* -and -not -path third_party/iec-60908b\* -and -not -path third_party/magic_enum\* -and -not -path third_party/PEGTL\* -and -not -path third_party/ucl\* -and -not -path third_party/args\* -and -not -path third_party/stb\* -exec git rm -f {} \; ; find src -maxdepth 1 -type d -and -not -name src -and -not -name mips-common -and -not -name support -and -not -name supportpsx -exec git rm -f {} \; || true' --tag-name-filter cat --prune-empty

# Delete ffmpeg, versionning, Lua, libuv and network source code (network needs libuv)
git filter-branch -f --tree-filter 'find src -type f -name \*lua\* -and -not -name xmake.lua -delete || true' --tag-name-filter cat --prune-empty
git filter-branch -f --tree-filter 'find src -type f -name assembler.\* -delete || true' --tag-name-filter cat --prune-empty
git filter-branch -f --tree-filter 'find src -type f -name ffmpeg\* -and -not -name ffmpeg-movie-file\* -delete || true' --tag-name-filter cat --prune-empty
git filter-branch -f --tree-filter 'find src -type f -name uv\* -delete || true' --tag-name-filter cat --prune-empty
git filter-branch -f --tree-filter 'find src -type f -name network\* -delete || true' --tag-name-filter cat --prune-empty
git filter-branch -f --tree-filter 'find src -type f -name version\* -delete || true' --tag-name-filter cat --prune-empty

# Inject our new root files.
git filter-branch -f --tree-filter "cp ${ROOT}/README-filtered.md README.md && cp ${ROOT}/LICENSE-filtered LICENSE && cp ${ROOT}/Makefile-filtered Makefile && mkdir -p .github/workflows && cp ${ROOT}/close.yml .github/workflows" --tag-name-filter cat
