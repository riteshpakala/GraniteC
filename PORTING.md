# Porting

GraniteC is Granite's agent kernel and the `granite-agent/1` standard in C, ported from the Swift sources
([riteshpakala/Granite](https://github.com/riteshpakala/Granite) at `8291f7a`) and the Rust port
([riteshpakala/GraniteRust](https://github.com/riteshpakala/GraniteRust) at `45583e4`, its event layer on Granite's
new design). It carries what an author needs to write an agent in C, and what a host needs to install, describe,
call and check one, in its own container or natively. Why something is different is
[Docs/Deviations.md](Docs/Deviations.md).

Status: **done** (ported, tested), **partly** (what a package needs of it), **not ported** (with why).

## Granite's core (Swift `Sources/Granite/Architecture/Core`, Rust `crates/granite/src`)

| Swift | Rust | C | Status |
| --- | --- | --- | --- |
| `Core/Concurrency/GraniteLock.swift` | `locked.rs` | `core/src/lock.c` (`granite_lock`, `granite_removal`, the held-lock check) | done |
| `Core/Concurrency/GraniteMain.swift` | `main_queue.rs` | `core/src/main.c` | done |
| `Core/Emitter/GraniteEmitter.swift` | `emitter.rs` | `core/src/emitter.c` | done, with displacement notices |
| ARC and `weak` | `Arc`, `Weak` | `core/src/object.c` | done |
| — | threads, `tokio` | `core/src/thread.c`, `core/src/buf.c` | done |
| `Core/Timing/GraniteDebounce.swift`, `GraniteThrottle.swift` | `reducer.rs` delivery | — | not ported: no reducers |
| `Core/Types/Inspectable.swift` | — | — | not ported: SwiftUI's |
| — | `state.rs`, `reducer.rs`, `service.rs`, `persist.rs` | — | not ported: a C agent is a package (Deviations 4) |
| — | `scaffolding/{lane,registry,tickets}.rs`, `testing.rs` | — | not ported: app scaffolding |

## GraniteAgents (Swift `Sources/GraniteAgents`, Rust `crates/granite/src/agent` and `crates/granite-agents/src`)

| Swift | Rust | C | Status |
| --- | --- | --- | --- |
| `Agent/AgentRunContainer.swift` | `agent/container.rs` | `agents/src/run_container.c` | partly: parallel lanes (Deviations 4) |
| `Agent/AgentContext.swift` | `agent/context.rs` | `agents/src/context.c` | partly: no `spawn` |
| `Agent/Values/AgentRunError.swift`, `AgentLifecycle.swift`, `AgentOutcome.swift` | `agent/values.rs` | `agents/src/runs.h`, the engine's sentences | partly |
| `Agent/Agent.swift`, `AgentExecutable.swift`, `GraniteAgent.swift` | `agent/executable.rs`, `agent/mod.rs` | `agents/src/package.c` (the package's host) | partly |
| `Agent/Values/AgentRunID.swift`, `AgentRunScope.swift`, `AgentConcurrency.swift`, `AgentStart.swift`, `ChildFrame.swift` | `agent/run_id.rs`, `agent/values.rs` | — | not ported: one policy, no children |
| `Host/AgentHost.swift`, `AgentHostModel.swift`, `AgentHostError.swift` | `agent/host.rs` | `agents/src/context.c`, `agents/src/error.c` | done |
| `Package/GraniteAgentPackage.swift`, `AnyAgentPackage.swift`, `AgentPackageRun.swift`, `AgentPackageHost.swift`, `AgentPackageCall.swift`, `AgentPackageCompute.swift`, `AgentPackageCenter.swift`, `AgentThrown.swift` | `package/{mod,any,run,host}.rs` | `agents/src/package.c` | done |
| `Package/AgentPackageError.swift`, `AgentRefusal.swift` | `package/mod.rs`, `refusal.rs` | `agents/src/error.c` | done |
| `Package/AgentMake.swift` | `make.rs` | `agents/include/granitec/answer.h` (`granite_make`) | done |
| `Answer/*.swift` | `answer/{mod,check}.rs` | `agents/src/answer.c` | done |
| `Schema/GraniteSchema.swift`, `GraniteSchema+Codable.swift` | `schema/mod.rs` | `agents/src/schema.c` | done, declared (Deviations 1) |
| `Schema/SchemaDecoder.swift` and its containers | `schema/probe.rs` | — | not ported: nothing to probe in C |
| `Wire/AgentWire*.swift`, `AgentJSON.swift`, `AgentSchemaJSON.swift` | `wire.rs`, `json.rs`, `entry.rs` | `agents/src/wire.c`, `agents/src/json.c` | done |
| `Roster/GraniteRoster.swift`, `GraniteRosterEntry*.swift` | `roster.rs` | `agents/src/package.c` (`granite_roster_*`) | done |
| `Roster/GraniteRoster+Discovery.swift` | — | — | not ported: rosters are listed (as in Rust) |
| `Channel/AgentChannel.swift`, `PipeChannel.swift` | `channel/{mod,pipe,memory}.rs` | `agents/src/channel.c` | done; a pair over pipes stands in for the memory channel |
| `Server/AgentServer.swift`, `AgentServerCall.swift`, `ChannelHostModel.swift` | `server/{mod,call,model}.rs` | `agents/src/server.c` | done |
| — | `host_ext.rs` | `granite_ask_schema` | done |
| `Process/AgentChild.swift`, `AgentProcess.swift`, `AgentProcessCost.swift`, `AgentRemoteError.swift` | `process/{child,mod,cost,error}.rs` | `host/src/{child,process,remote}.c` | done |
| `Remote/RemoteAgent.swift`, `RemoteLanes.swift` | `remote.rs` | — | not ported: the `granite` command calls a runner directly |
| `Shelf/AgentShelf.swift` | `shelf.rs` | `host/src/shelf.c`, `host/src/sha256.c` | done |
| `Install/AgentInstaller.swift`, `AgentInstallError.swift`, `AgentCommand.swift`, `AgentSource.swift`, `AgentInstallation.swift` | `install/{mod,error,command,source,installation}.rs` | `host/src/{install,command,installation}.c` | done, for C repositories (Deviations 11) |
| `Install/PackageManifest.swift` | `install/manifest.rs` | `host/src/install.c` (`granite-agent.json`) | done, C's manifest |
| `Worker/*.swift` | `worker.rs` | — | not ported: worker specs are a host's own |
| `Scaffolding/*.swift` | `scaffolding/*.rs` | — | not ported: app scaffolding |

## The command, the checks, the containers

| Rust | C | Status |
| --- | --- | --- |
| `crates/granite-cli/src/main.rs` | `cli/bin/granite.c` | done, byte for byte (`cli/tests`) |
| `jsonschema` (crate) | `jsonschema/src/validate.c` | done, the standard's subset (Deviations 15) |
| `crates/granite-conformance` (parrot) | `tests/fixtures/agent-repo/parrot.c` | done, its `--describe` the Rust parrot's bytes |
| `fixtures/hello-agent` | `tests/fixtures/agent-repo` | done |
| `containers/{Dockerfile,Dockerfile.linux,check.sh}`, `scripts/{check,conformance-linux}.sh` | the same names | written; the Linux ones not run here ([Docs/Containers.md](Docs/Containers.md)) |
| `crates/granite-agents/tests/schema_drift.rs` | `make drift-check`, `jsonschema/tests` | done |
| — | `scripts/{json,wire,check,cli}-goldens.sh`, `scripts/interop.sh` | GraniteC's: goldens taken from serde_json and GraniteRust, and interop |
