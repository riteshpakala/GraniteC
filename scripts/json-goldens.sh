#!/bin/sh
# Regenerates agents/tests/json/serde.expected: what serde_json 1.0.154 (GraniteRust's) makes of
# every case in corpus.hex. Needs cargo. Run it after changing make_corpus.py.
set -eu
cd "$(dirname "$0")/.."
python3 agents/tests/json/make_corpus.py
CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-build/serde-golden}" cargo run --quiet --release \
    --manifest-path scripts/serde-golden/Cargo.toml < agents/tests/json/corpus.hex > agents/tests/json/serde.expected
echo "json-goldens: $(wc -l < agents/tests/json/serde.expected | tr -d ' ') cases"
