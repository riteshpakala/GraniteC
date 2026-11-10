#!/usr/bin/env python3
"""Writes cases.jsonl: the granite command's cases, each its arguments and its stdin. {PARROT} is
the parrot runner, {SCHEMA} this repository's Schema/, {SHELF} an empty shelf. rust.expected is what
GraniteRust's granite says to each, normalized; scripts/cli-goldens.sh regenerates it."""
import json, os

entry = {"fields": {"text": "what to say"}, "kind": "package", "lanes": 2, "module": "M",
         "schema": {"object": [{"name": "text", "required": True, "schema": "string"}]},
         "summary": "says it back.", "type": "M.T", "word": "parrot"}
cases = [
    {"args": [], "stdin": ""},
    {"args": ["frobnicate"], "stdin": ""},
    {"args": ["agents", "list", "--shelf"], "stdin": ""},
    {"args": ["agents", "list"], "stdin": ""},
    {"args": ["agents", "list", "--shelf", "{SHELF}"], "stdin": ""},
    {"args": ["agents", "list", "--json", "--shelf", "{SHELF}"], "stdin": ""},
    {"args": ["agents", "--shelf", "{SHELF}"], "stdin": ""},
    {"args": ["agents", "rm", "nobody", "--shelf", "{SHELF}"], "stdin": ""},
    {"args": ["agents", "update", "nobody", "--shelf", "{SHELF}"], "stdin": ""},
    {"args": ["describe", "{PARROT}"], "stdin": ""},
    {"args": ["describe", "/nowhere/runner"], "stdin": ""},
    {"args": ["run", "{PARROT}", "-"], "stdin": json.dumps({"perform": {"word": "parrot", "input": {"text": "hi", "notes": 2},
                                                                        "ask": "say", "directory": "/tmp"}})},
    {"args": ["run", "{PARROT}", "-", "--word", "parrot", "--ask", "speak"], "stdin": json.dumps({"input": {"text": "x"}})},
    {"args": ["run", "{PARROT}", "-"], "stdin": json.dumps({"word": "parrot", "input": {"text": "x", "fail": True}})},
    {"args": ["run", "{PARROT}", "-"], "stdin": json.dumps({"word": "parrot", "input": {"text": "x", "fault": True}})},
    {"args": ["run", "{PARROT}", "-"], "stdin": json.dumps({"word": "parrot", "input": {"text": "x", "crash": True}})},
    {"args": ["run", "{PARROT}", "-"], "stdin": json.dumps({"word": "parrot", "input": {"text": "x", "askModel": True}})},
    {"args": ["run", "{PARROT}", "-"], "stdin": json.dumps({"word": "parrot", "input": {}})},
    {"args": ["run", "{PARROT}", "-"], "stdin": json.dumps({"word": "parrot"})},
    {"args": ["run", "{PARROT}", "-", "--word", "comic"], "stdin": "{}"},
    {"args": ["run", "{PARROT}", "-"], "stdin": json.dumps({"input": {}})},
    {"args": ["run", "{PARROT}", "nope.json"], "stdin": ""},
    {"args": ["run", "{PARROT}", "-"], "stdin": "x"},
    {"args": ["run", "/nowhere/runner", "-"], "stdin": json.dumps({"word": "parrot", "input": {"text": "x"}})},
    {"args": ["check", "--schemas", "{SCHEMA}", "--schema", "granite-agent-entry.schema.json", "-"], "stdin": json.dumps(entry)},
    {"args": ["check", "--schemas", "{SCHEMA}", "--schema", "granite-agent-entry.schema.json", "-"],
     "stdin": json.dumps(dict(entry, word="Not A Word", lanes=0))},
    {"args": ["check", "--schemas", "{SCHEMA}", "-"], "stdin": "{}"},
    {"args": ["check", "--schemas", "{SCHEMA}", "--schema", "nope.json", "-"], "stdin": "{}"},
    {"args": ["check", "--schemas", "{SCHEMA}", "--schema", "granite-agent-entry.schema.json", "-"], "stdin": "[1,"},
    {"args": ["check", "--schemas", "/nowhere/schemas", "--schema", "x", "-"], "stdin": "{}"},
]
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cases.jsonl'), 'w') as out:
    for case in cases:
        out.write(json.dumps(case, sort_keys=True) + '\n')
print(f'{len(cases)} cases')
