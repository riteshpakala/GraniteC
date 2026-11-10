#!/usr/bin/env python3
"""GraniteC against GraniteRust, natively: every host with every runner, the same bytes.

  1. describe: Rust's granite and C's, each describing Rust's parrot and C's: one output.
  2. run: every case, through each host and each runner, against Rust's granite with Rust's parrot:
     the same line, status, notes and exit code (pids and costs made the same). A case whose input does
     not read differs only in serde's typed sentence, which C does not say (Docs/Deviations.md).
  3. trace: what each host sends a runner (GRANITE_AGENT_TRACE=1), byte for byte.
  4. shelves: Rust's granite checks and lists a shelf C's installer wrote, and C's granite lists one
     Rust's installer wrote (GraniteRust's hello agent, built in release: a minute).
  5. mirror: every value of the JSON corpus through a C runner (tests/fixtures/mirror_runner), equal to
     what serde_json writes of the same text.

  scripts/interop.sh <GraniteC build folder> <GraniteRust checkout> [--quick]   (--quick skips step 4's Rust build)
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

build, rust = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
quick = '--quick' in sys.argv
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
cargo = shutil.which('cargo') or os.path.expanduser('~/.cargo/bin/cargo')
problems = []


def say(text):
    print(text, flush=True)


def build_rust():
    say('building GraniteRust\'s granite and parrot')
    subprocess.run([cargo, 'build', '-q', '-p', 'granite-cli', '-p', 'granite-conformance', '--bin', 'parrot-runner'],
                   cwd=rust, check=True)


build_rust()
hosts = {'rust': f'{rust}/target/debug/granite', 'c': f'{build}/bin/granite'}
runners = {'rust': f'{rust}/target/debug/parrot-runner', 'c': f'{build}/fixtures/parrot_runner'}
work = tempfile.mkdtemp(prefix='granitec-interop.')
environment = {k: v for k, v in os.environ.items() if k not in ('GRANITE_SHELF', 'GRANITE_AGENT_TRACE')}


def normalize(text):
    for name, path in runners.items():
        text = text.replace(path, '{PARROT}')
    text = text.replace(work, '{WORK}')
    text = re.sub(r'"pid":\d+', '"pid":1', text)
    text = re.sub(r'as pid \d+', 'as pid 1', text)
    text = re.sub(r'\[granite-agent \d+\]', '[granite-agent 1]', text)
    return re.sub(r'released · peak [0-9.]+ [MG]B · [0-9.]+ s CPU in [0-9.]+ s', 'released · <cost>', text)


def granite(host, args, stdin='', extra=None):
    ran = subprocess.run([hosts[host]] + args, input=stdin, cwd=work, capture_output=True, text=True,
                         env=dict(environment, **(extra or {})))
    return ran.returncode, normalize(ran.stdout), normalize(ran.stderr)


# 1. describe -------------------------------------------------------------------------------------
say('1. describe')
reference = granite('rust', ['describe', runners['rust']])
for host in hosts:
    for runner in runners:
        said = granite(host, ['describe', runners[runner]])
        if said != reference:
            problems.append(f'describe: {host} host, {runner} runner: {said} != {reference}')
say(f'   four ways, one output ({len(reference[1])} bytes)')

# 2. run ------------------------------------------------------------------------------------------
say('2. run')
cases = [
    ({'word': 'parrot', 'input': {'text': 'hi', 'notes': 2}, 'ask': 'say', 'directory': '/tmp'}, False),
    ({'word': 'parrot', 'input': {'text': 'x', 'fail': True}}, False),
    ({'word': 'parrot', 'input': {'text': 'x', 'fault': True}}, False),
    ({'word': 'parrot', 'input': {'text': 'x', 'crash': True}}, False),
    ({'word': 'parrot', 'input': {'text': 'x', 'askModel': True}}, False),
    ({'word': 'parrot', 'input': {'text': 'x', 'delay': 0.2}}, False),
    ({'word': 'comic', 'input': {}}, False),
    ({'word': 'parrot', 'input': {'text': 'é "quoted" / slashed', 'directory': 'rel'}}, False),
    ({'word': 'parrot', 'input': {}}, True),
    ({'word': 'parrot', 'input': {'text': 5}}, True),
    ({'word': 'parrot'}, True),
]
for perform, typed in cases:
    stdin = json.dumps({'perform': perform})
    reference = granite('rust', ['run', runners['rust'], '-'], stdin)
    for host in hosts:
        for runner in runners:
            said = granite(host, ['run', runners[runner], '-'], stdin)
            if said == reference:
                continue
            if typed and runner == 'c':
                # serde's typed sentence ("… at line 1 column 2") is Rust's own: compare the rest.
                line, rust_line = json.loads(said[1]), json.loads(reference[1])
                same = (said[0], line.get('refused', {}).get('kind'), line.get('status')) == \
                       (reference[0], rust_line.get('refused', {}).get('kind'), rust_line.get('status'))
                if same:
                    continue
            problems.append(f'run {perform}: {host} host, {runner} runner:\n  {said}\n  != {reference}')
say(f'   {len(cases)} cases, through each host and each runner')

# 3. trace ----------------------------------------------------------------------------------------
say('3. trace')
stdin = json.dumps({'perform': {'word': 'parrot', 'input': {'text': 'traced'}, 'ask': 'a/b', 'directory': '/tmp'}})
traces = {}
for host in hosts:
    _, _, err = granite(host, ['run', runners['c'], '-'], stdin, {'GRANITE_AGENT_TRACE': '1'})
    traces[host] = [line for line in err.splitlines() if '] sent ' in line or '] read ' in line]
if traces['rust'] != traces['c']:
    problems.append(f'trace: {traces["c"]} != {traces["rust"]}')
say(f'   {len(traces["rust"])} lines, the same from each host')

# 4. shelves --------------------------------------------------------------------------------------
say('4. shelves')
c_shelf = f'{work}/c-shelf'
code, out, err = granite('c', ['agents', 'add', f'{root}/tests/fixtures/agent-repo', '--shelf', c_shelf],
                         extra={'PKG_CONFIG_PATH': f'{build}/pkgconfig'})
if code != 0:
    problems.append(f'C install: {err}')
code, out, err = granite('rust', ['check', '--schemas', f'{root}/Schema', '--schema', 'granite-agent-installation.schema.json',
                                  f'{c_shelf}/parrot-c/installation.json'])
if code != 0:
    problems.append(f'Rust checks C\'s installation.json: {err}')
for flags in ([], ['--json']):
    rust_list, c_list = granite('rust', ['agents', 'list', '--shelf', c_shelf] + flags), granite('c', ['agents', 'list', '--shelf', c_shelf] + flags)
    if rust_list != c_list:
        problems.append(f'list of C\'s shelf {flags}: {c_list} != {rust_list}')
if not quick:
    rust_shelf = f'{work}/rust-shelf'
    code, out, err = granite('rust', ['agents', 'add', f'{rust}/fixtures/hello-agent', '--shelf', rust_shelf])
    if code != 0:
        problems.append(f'Rust install: {err[-500:]}')
    for flags in ([], ['--json']):
        rust_list, c_list = granite('rust', ['agents', 'list', '--shelf', rust_shelf] + flags), granite('c', ['agents', 'list', '--shelf', rust_shelf] + flags)
        if rust_list != c_list:
            problems.append(f'list of Rust\'s shelf {flags}: {c_list} != {rust_list}')
    installed = json.load(open(f'{rust_shelf}/hello-agent/installation.json')) if os.path.exists(f'{rust_shelf}/hello-agent/installation.json') else None
    if installed:
        word = installed['entries'][0]['word']
        runner = f'{rust_shelf}/hello-agent/agent'
        for args in (['describe', runner], ['run', runner, '-']):
            stdin = json.dumps({'word': word, 'input': {'name': 'interop'}}) if args[0] == 'run' else ''
            rust_said, c_said = granite('rust', args, stdin), granite('c', args, stdin)
            if rust_said != c_said:
                problems.append(f'{args[0]} of Rust\'s hello agent: {c_said} != {rust_said}')
    say('   Rust reads C\'s shelf, C reads Rust\'s, and both drive Rust\'s hello agent alike')
else:
    say('   Rust reads C\'s shelf (Rust\'s installer skipped: --quick)')

# 5. mirror ---------------------------------------------------------------------------------------
# A runner reads its input three times over: the wire line, the input written for the package and read
# again, and the answer written, read and written onto the wire. GraniteRust's runner does the same
# with serde, whose default float reading is not exact, so a double may move at each round: what C
# must write is serde's read-and-write applied three times (scripts/serde-golden, pinned to Rust's).
say('5. mirror')
corpus = open(f'{root}/agents/tests/json/corpus.hex').read().split('\n')[:-1]
serde_target = f'{work}/serde-golden'
golden = [cargo, 'run', '-q', '--release', '--manifest-path', f'{root}/scripts/serde-golden/Cargo.toml']
rounds = corpus
for _ in range(3):
    ran = subprocess.run(golden, input='\n'.join(rounds) + '\n', capture_output=True, text=True, check=True,
                         env=dict(environment, CARGO_TARGET_DIR=f'{build}/serde-golden'))
    said = ran.stdout.split('\n')[:-1]
    rounds = [line.split(' ')[1] if line.startswith('ok ') else '' for line in said]
expected = ['ok ' + r if r else 'err' for r in rounds]
mirrored = 0
for case, want in zip(corpus, expected):
    parts = want.split(' ')
    value = bytes.fromhex(case)
    # A wire line holds no raw newline, and the perform around the value adds three levels.
    if parts[0] != 'ok' or b'\n' in value or value.count(b'[') + value.count(b'{') > 100:
        continue
    line = b'{"perform":{"input":{"value":' + value + b'},"model":false,"word":"mirror"}}\n'
    ran = subprocess.run([f'{build}/fixtures/mirror_runner'], input=line, capture_output=True)
    output = [l for l in ran.stdout.split(b'\n') if l.startswith(b'{"output":')]
    serde = bytes.fromhex(parts[1])
    found = re.search(rb'"mirrored":(.*),"path":', output[0]) if output else None
    if not found or found.group(1) != serde:
        problems.append(f'mirror {value[:60]!r}: {found.group(1)[:80] if found else ran.stdout[:200]!r} != {serde[:80]!r}')
    mirrored += 1
say(f'   {mirrored} values, each written as serde writes it')

shutil.rmtree(work, ignore_errors=True)
if problems:
    say(f'\ninterop: {len(problems)} problems')
    for problem in problems:
        say(f'- {problem}')
    sys.exit(1)
say('\ninterop: passed')
