#!/bin/bash

function clean() {
  f="$1"
  fromdos "$f"
  tabremover -t 8 "$f" "$f".2
  mv "$f.2" "$f"
}

function format() {
  f="$1"
  if [ "${f##*.}" != "lua" ]; then
    clang-format --style=file "$f" > "$f.2"
  else
    lua-format "$f" --config=/lua-format.config > "$f.2"
  fi
  if [ $? -ne 0 ] || { [ ! -s "$f.2" ] && [ -s "$f" ]; }; then
    echo "formatter failed on $f, keeping its unformatted contents" >&2
    rm -f "$f.2"
    return
  fi
  mv "$f.2" "$f"
  sed "s/ *$//" -i "$f"
}

# src/mips is the nugget submodule, formatted in its own repository.
find repository/src repository/tests -path repository/src/mips -prune -o \( -name '*.c' -or -name '*.cc' -or -name '*.cpp' -or -name '*.h' -or -name '*.hh' -or -name '*.lua' \) -print | while read f ; do
  echo "$f"
  clean "$f"
  format "$f"
done

find repository/src repository/tests -path repository/src/mips -prune -o \( -name '*.s' -or -name '*.S' \) -print | while read f ; do
  echo "$f"
  clean "$f"
done

find repository/vsprojects -name *.vcxproj | while read f ; do
  echo "$f"
  clean "$f"
done

find repository/i18n -type f | while read f ; do
  echo "$f"
  clean "$f"
done
