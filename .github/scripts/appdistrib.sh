#!/bin/bash
# Runs the AppDistrib CLI from a pinned commit of AppDistrib/AppDistrib, which is ahead of the npm
# release: it keeps several upload chunks in flight, so an upload spends far fewer round trips with
# the stream open. Arguments pass through, and stdout is only the CLI's (buildid prints JSON).
set -euo pipefail
rev=ddf277769fce0bac8b1dcf79b729926f81c41083
dir="${RUNNER_TEMP:-/tmp}/appdistrib-cli-$rev"
if [ ! -f "$dir/.installed" ]; then
  rm -rf "$dir"
  mkdir -p "$dir"
  # node rather than curl: the Linux build container has no curl.
  node -e 'fetch(process.argv[1]).then(async (r) => { if (!r.ok) throw new Error("HTTP " + r.status); process.stdout.write(Buffer.from(await r.arrayBuffer())) }).catch((e) => { console.error(e); process.exit(1) })' \
    "https://codeload.github.com/AppDistrib/AppDistrib/tar.gz/$rev" |
    tar xz -C "$dir" --strip-components=3 "AppDistrib-$rev/src/client"
  (cd "$dir" && npm ci --omit=dev --no-audit --no-fund --loglevel=error >&2)
  touch "$dir/.installed"
fi
exec node "$dir/index.js" "$@"
