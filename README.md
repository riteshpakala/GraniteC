# GraniteC

Granite's agent kernel and the `granite-agent/1` standard in C: what you link to write an agent in C, and the
`granite` command a host uses to install, describe, run and check one. The Swift original is
[Granite](https://github.com/riteshpakala/Granite); the Rust port is [GraniteRust](https://github.com/riteshpakala/GraniteRust).
The wire is language-neutral, so a C host drives Swift and Rust runners and the other way around.

```sh
make            # libgranitec.a and the granite command, under build/
make test       # every package's tests
```

Needs a C11 compiler with pthreads, GNU make (3.81 or later), pkg-config and json-c 0.15 or later.
`PORTING.md` maps each Swift and Rust file to its C home.
