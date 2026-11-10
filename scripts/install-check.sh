#!/bin/sh
# Installs GraniteC into a scratch prefix, then builds the fixture agent repository against that
# install through pkg-config alone, as `granite agents add` would on a machine with GraniteC on it.
# Checks the built runner's --describe is the Rust parrot's bytes, that it answers a call, and that
# the installed granite checks its entry against the installed schemas.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/granitec-install-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
prefix="$scratch/prefix"

make -s install PREFIX="$prefix" >/dev/null
echo "install-check: installed into $prefix"

# Only the installed granitec.pc is on the path: not this build tree's uninstalled one.
PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH
[ "$(pkg-config --modversion granitec)" = "$(sed -n 's/^VERSION := //p' Makefile)" ]
pkg-config --variable=commit granitec >/dev/null
env -u MAKEFLAGS -u MAKELEVEL make -s -C tests/fixtures/agent-repo O="$scratch/build" agent
runner="$scratch/build/agent"

"$runner" --describe | cmp - tests/fixtures/parrot.describe
echo "install-check: the installed build's --describe is the Rust parrot's bytes"

printf '%s\n' '{"perform":{"word":"parrot","input":{"text":"installed"},"model":false}}' | "$runner" 2>/dev/null \
    | grep -q '"said":"installed"'
echo "install-check: it answers a call"

"$runner" --describe | sed -e 's/^{"entries":\[//' -e 's/\]}$//' > "$scratch/entry.json"
"$prefix/bin/granite" check --schemas "$prefix/share/granitec/schemas" \
    --schema granite-agent-entry.schema.json "$scratch/entry.json" 2>/dev/null
echo "install-check: the installed granite checks its entry against the installed schemas"

make -s uninstall PREFIX="$prefix"
leftover=$(find "$prefix" -type f | wc -l | tr -d ' ')
[ "$leftover" = 0 ] || { echo "install-check: uninstall left $leftover files"; exit 1; }
echo "install-check: passed"
