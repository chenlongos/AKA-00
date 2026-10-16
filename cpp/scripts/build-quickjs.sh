#!/bin/sh
# Pinned source dependency; neither the board nor normal builds need network access.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
quickjs_root="$script_dir/../third_party/quickjs"
quickjs_version=2026-06-04
quickjs_sha=b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a
if [ -f "$quickjs_root/quickjs.c" ] && [ "$(cat "$quickjs_root/.source-sha256" 2>/dev/null)" = "$quickjs_sha" ]; then
    exit 0
fi
quickjs_tmp=$(mktemp -d)
trap 'rm -rf "$quickjs_tmp"' EXIT HUP INT TERM
curl -fL --retry 2 --connect-timeout 10 --max-time 120 \
    "https://bellard.org/quickjs/quickjs-$quickjs_version.tar.xz" -o "$quickjs_tmp/source.tar.xz"
printf '%s  %s\n' "$quickjs_sha" "$quickjs_tmp/source.tar.xz" | sha256sum -c -
tar -xJf "$quickjs_tmp/source.tar.xz" -C "$quickjs_tmp"
mkdir -p "$(dirname -- "$quickjs_root")"
# Never nest a newly extracted version inside an existing dependency directory.
if [ -e "$quickjs_root" ]; then
    echo "QuickJS source exists with a different checksum marker: $quickjs_root" >&2
    exit 1
fi
mv "$quickjs_tmp/quickjs-$quickjs_version" "$quickjs_root"
printf '%s\n' "$quickjs_sha" > "$quickjs_root/.source-sha256"
