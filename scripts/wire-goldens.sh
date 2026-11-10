#!/bin/sh
# Regenerates agents/tests/wire/rust.expected: what GraniteRust's AgentWire::read makes of every
# line in corpus.hex. Needs cargo and a GraniteRust checkout beside this one.
set -eu
cd "$(dirname "$0")/.."
python3 agents/tests/wire/make_corpus.py
CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-build/wire-golden}" cargo run --quiet --release \
    --manifest-path scripts/wire-golden/Cargo.toml < agents/tests/wire/corpus.hex > agents/tests/wire/rust.expected
echo "wire-goldens: $(wc -l < agents/tests/wire/rust.expected | tr -d ' ') lines"
