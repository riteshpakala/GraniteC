#!/bin/sh
# Regenerates jsonschema/tests/check/rust.expected: what GraniteRust's `granite check` says of every
# case in cases.jsonl, against this repository's Schema/ and containers/schemas/. Needs a built
# GraniteRust beside this checkout (cargo build -p granite-cli) or RUST_GRANITE=<its granite>.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
granite=${RUST_GRANITE:-$root/../GraniteRust/target/debug/granite}
python3 jsonschema/tests/check/make_cases.py
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$granite" "$root" "$work" <<'PY'
import json, subprocess, sys
granite, root, work = sys.argv[1:4]
out = open(f'{root}/jsonschema/tests/check/rust.expected', 'w')
for line in open(f'{root}/jsonschema/tests/check/cases.jsonl'):
    case = json.loads(line)
    with open(f'{work}/instance.json', 'w') as f:
        json.dump(case['instance'], f)
    ran = subprocess.run([granite, 'check', '--schemas', f'{root}/Schema', '--schemas', f'{root}/containers/schemas',
                          '--schema', case['schema'], 'instance.json'], cwd=work, capture_output=True, text=True)
    out.write(json.dumps({'code': ran.returncode, 'stderr': ran.stderr}, sort_keys=True) + '\n')
PY
echo "check-goldens: $(wc -l < jsonschema/tests/check/rust.expected | tr -d ' ') cases"
