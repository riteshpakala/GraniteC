# Porting

GraniteC is Granite's agent kernel and the `granite-agent/1` standard in C, ported from the Swift sources
(`riteshpakala/Granite`) and the Rust port (`riteshpakala/GraniteRust`). It carries what an author needs to write an
agent in C, and what a host needs to install, describe, call and check one in its own container. It is filled in
package by package; this file maps every Swift and Rust file to its C home, or says it was not ported, as each lands.

| Package | What it is | Status |
| --- | --- | --- |
| `Schema/` | the standard and its four JSON Schemas, verbatim | copied |
| `testing/` | the shared test harness (`granite_test.h`) | done |
| `core/` | objects, the lock, the emitter, the main queue | to do |
| `agents/` | JSON, errors, contexts, the run engine, packages, the wire, the runner | to do |
| `host/` | the host's side of a call, the shelf, the installer | to do |
| `jsonschema/` | the subset validator behind `granite check` | to do |
| `cli/` | the `granite` command | to do |
