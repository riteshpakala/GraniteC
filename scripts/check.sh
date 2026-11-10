#!/bin/sh
# The repo's one check: warnings as errors and every test, plain, under ASan+UBSan and under TSan;
# the lint; and an install that builds the fixture agent through pkg-config.
set -eu
cd "$(dirname "$0")/.."
make WERROR=1 all test
make WERROR=1 SAN=address,undefined test
make WERROR=1 SAN=thread test
make lint
make install-check
echo "granitec: check passed"
