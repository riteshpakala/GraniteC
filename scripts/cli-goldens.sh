#!/bin/sh
# Regenerates cli/tests/rust.expected: what GraniteRust's granite says to every case in
# cli/tests/cases.jsonl, run against this build's C parrot (whose bytes are the Rust parrot's),
# normalized as cli/tests/normalize.py normalizes. Needs a built GraniteRust beside this checkout
# (cargo build -p granite-cli) or RUST_GRANITE=<its granite>, and `make` run here first.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
granite=${RUST_GRANITE:-$root/../GraniteRust/target/debug/granite}
python3 cli/tests/make_cases.py
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/shelf"
python3 - "$granite" "$root" "$work" <<'PY'
import json, os, subprocess, sys
sys.path.insert(0, sys.argv[2] + '/cli/tests')
from normalize import normalize
granite, root, work = sys.argv[1:4]
places = {'PARROT': f'{root}/build/fixtures/parrot_runner', 'SCHEMA': f'{root}/Schema', 'SHELF': f'{work}/shelf'}
environment = {k: v for k, v in os.environ.items() if k != 'GRANITE_SHELF'}
out = open(f'{root}/cli/tests/rust.expected', 'w')
for line in open(f'{root}/cli/tests/cases.jsonl'):
    case = json.loads(line)
    args = [a for arg in case['args'] for a in [arg.format(**places)]]
    ran = subprocess.run([granite] + args, input=case['stdin'], cwd=work, capture_output=True, text=True, env=environment)
    out.write(json.dumps({'code': ran.returncode, 'stdout': normalize(ran.stdout, places),
                          'stderr': normalize(ran.stderr, places)}, sort_keys=True) + '\n')
PY
echo "cli-goldens: $(wc -l < cli/tests/rust.expected | tr -d ' ') cases"
