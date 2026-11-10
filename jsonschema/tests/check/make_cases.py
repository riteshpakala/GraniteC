#!/usr/bin/env python3
"""Writes cases.jsonl: the instances `granite check` is held to, each against a schema named by file
name or $id. rust.expected is what GraniteRust's `granite check` says of each (its exit code and its
problems, one per line); scripts/check-goldens.sh regenerates it."""
import copy
import json
import os

here = os.path.dirname(os.path.abspath(__file__))
WIRE, ENTRY, ANSWER, INSTALL = (f'granite-agent-{n}.schema.json' for n in ('wire', 'entry', 'answer', 'installation'))
CRAFT = 'craft-agent/1/output'
ANSWER_ID = 'https://github.com/riteshpakala/Granite/blob/main/Schema/granite-agent-answer.schema.json'

entry = {"fields": {"text": "what to say"}, "guide": "Give it text.", "kind": "package", "lanes": 2,
         "module": "granite_conformance", "schema": {"object": [
             {"name": "text", "required": True, "schema": "string"},
             {"name": "mood", "required": False, "schema": {"choice": ["calm", "loud"]}},
             {"name": "tags", "required": False, "schema": {"array": "string"}},
             {"name": "any", "required": False, "schema": "any"}]},
         "summary": "says it back.", "type": "granite_conformance.Parrot", "word": "parrot",
         "makes": [{"kind": "card", "what": "a card", "into": ["cards/"]}]}
answer = {"agent": {"word": "parrot"}, "format": "granite-answer/1", "path": "cards/a.json", "paths": ["cards/a.json"],
          "made": [{"kind": "card", "name": "a", "path": "cards/a.json", "state": "written"}],
          "expected": [{"kind": "card", "name": "a"}], "problems": [{"kind": "partial", "text": "one left"}],
          "payloads": [{"path": "cards/a.json", "schema": "craft-timeseries/1"}], "notes": ["made one"], "said": "hi"}
installation = {"builtAt": "2026-11-09T12:00:00Z", "entries": [entry], "executable": "agent",
                "format": "granite-agent-installation/1", "granite": "0.1.0@abc", "modified": 1762689600.5,
                "name": "parrot-c", "revision": "f" * 40, "sha256": "a" * 64, "size": 1234,
                "source": {"path": "/src/parrot"}}

def mutated(base, change):
    value = copy.deepcopy(base)
    change(value)
    return value

def delete(path):
    def change(value):
        *parents, last = path
        for key in parents:
            value = value[key]
        del value[last]
    return change

def put(path, new):
    def change(value):
        *parents, last = path
        for key in parents:
            value = value[key]
        value[last] = new
    return change

cases = []
add = lambda schema, instance: cases.append({"schema": schema, "instance": instance})

for message in [
    {"hello": {"pid": 7, "wire": "granite-agent/1", "answer": "granite-answer/1"}},
    {"perform": {"word": "parrot", "input": {"text": "hi"}, "model": False, "directory": "/p", "ask": "say"}},
    {"note": "round 1"}, {"model": {"id": 1, "kind": "object", "schema": {"type": "object"}, "system": "s", "user": "u",
                                    "maxTokens": 300, "seconds": 30}},
    {"answer": {"id": 1, "object": {"n": 4}}}, {"output": answer}, {"failed": "told to fail", "refused": {
        "kind": "call", "reasons": ["leave out `fail`"]}}, {"cancel": {}}, {"entries": [entry]},
]:
    add(WIRE, message)
for broken in [
    {"hello": {"wire": "granite-agent/x"}}, {"hello": {"pid": "7", "wire": 1}}, {"perform": {"word": "x"}},
    {"model": {"id": 1.5, "kind": "image", "system": "s", "user": "u", "maxTokens": 3, "seconds": "2"}},
    {"failed": "x", "refused": {"kind": "weather", "reasons": [1]}}, {"failed": "x", "cancel": {}},
    {"weather": {}}, {}, [], "hello", {"output": [1]}, {"entries": [{"word": "x"}]},
]:
    add(WIRE, broken)

add(ENTRY, entry)
for change in [delete(["word"]), put(["word"], "Not A Word"), put(["lanes"], 0), put(["kind"], "robot"),
               put(["schema"], {"object": [{"name": "x", "schema": "strings", "required": True}]}),
               put(["schema"], {"choice": "calm"}), put(["makes"], [{"kind": "card"}]), put(["fields"], {"text": 5}),
               delete(["summary"]), put(["schema"], 7)]:
    add(ENTRY, mutated(entry, change))

add(ANSWER, answer)
add(ANSWER_ID, answer)
for change in [delete(["paths"]), put(["format"], "granite-answer/2"), put(["made", 0, "state"], "torn"),
               put(["agent"], {}), put(["paths"], [1]), put(["path"], 5), put(["problems", 0, "kind"], "Two Words"),
               put(["payloads", 0], {"path": "x"}), put(["notes"], "one"), put(["expected", 0], {"kind": 3})]:
    add(ANSWER, mutated(answer, change))

add(INSTALL, installation)
add(INSTALL, mutated(installation, put(["source"], {"url": "https://x/y.git", "revision": "abc"})))
for change in [delete(["sha256"]), put(["sha256"], "XYZ"), put(["size"], -1), put(["format"], "nope"),
               put(["source"], {"neither": 1}), put(["entries"], [{"kind": "package"}]), put(["builtAt"], 5),
               put(["extra"], True), put(["modified"], "now")]:
    add(INSTALL, mutated(installation, change))

add(CRAFT, answer)
add(CRAFT, mutated(answer, put(["payloads"], [{"path": "cards/a.json"}])))
add(CRAFT, mutated(answer, delete(["format"])))
add(CRAFT, mutated(answer, put(["payloads"], [{"path": 1, "schema": "craft-timeseries/1"}])))

with open(os.path.join(here, 'cases.jsonl'), 'w') as out:
    for case in cases:
        out.write(json.dumps(case, sort_keys=True) + '\n')
print(f'{len(cases)} cases')
