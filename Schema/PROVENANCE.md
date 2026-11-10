# Provenance

Copied verbatim from [riteshpakala/Granite](https://github.com/riteshpakala/Granite) at `cbae888c8098981964ac0fe6eaee9cfcb9baa578`
(2026-11-04): `GraniteAgent.md` and the four generated JSON Schemas. They are the same bytes GraniteRust carries. The
standard changes in Granite first; `make drift-check GRANITE_DIR=../Granite` says when these copies have fallen behind,
and the suite holds the code to them (every message, entry, answer and installation GraniteC writes validates against
its schema). To refresh: copy the five files again and run `make test`.
