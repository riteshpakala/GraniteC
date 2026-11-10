#!/bin/sh
# GraniteC against GraniteRust, natively (scripts/interop.py says what is compared).
#   scripts/interop.sh <GraniteC build folder> <GraniteRust checkout> [--quick]
set -eu
exec python3 "$(dirname "$0")/interop.py" "$@"
