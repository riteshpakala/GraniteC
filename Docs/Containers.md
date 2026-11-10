# Containers

`containers/check.sh` installs a C agent repository and checks what its runner says against the standard:

```sh
containers/check.sh <agent folder | git URL[@revision]>            # in an Apple container (Linux, arm64)
containers/check.sh tests/fixtures/agent-repo --native             # on this machine
containers/check.sh ../ComicAgent --granite ../GraniteC --out /tmp/comic-check
```

1. **Install** it with GraniteC's installer; `installation.json` must keep `granite-agent-installation.schema.json`.
2. **Describe** it; every entry must keep `granite-agent-entry.schema.json`.
3. **Call** it with every fixture in its `Schema/examples/`: a `*.perform.json` must be answered (the answer keeping
   Granite's answer schema and Craft's output schema, stamped, every path there, only kinds it declares made, every
   `expected` made); a `*.refusal.json` must be refused with its kind.

In a container, the image is `containers/Dockerfile`: Debian bookworm, build-essential, pkg-config, libjson-c-dev,
git and jq, with GraniteC installed into `/usr/local` (its commit passed as a build argument, since `.git` is not in
the build context). Natively, GraniteC is installed from this checkout (or `--granite <clone>`) into a scratch prefix
and found through `PKG_CONFIG_PATH`. A folder that is the top of its own repository is cloned at its last commit; one
inside another repository is copied.

`scripts/container.sh` runs the `granite` command in the check image with the current folder at `/scratch`.
`containers/Dockerfile.linux` and `scripts/conformance-linux.sh` run `scripts/check.sh`, GraniteC's own suite, on
Linux.

## What has run, and what has not

Verified on macOS 27 (arm64), Apple clang 21, json-c 0.19, make 3.81 and 4.4: the whole suite plain, under
ASan+UBSan and under TSan; the install check; `containers/check.sh tests/fixtures/agent-repo --native`; and interop
with GraniteRust.

Written and not yet run, because Apple's `container` (1.6.1, unpacked under `~/.local/apple-container`) has not been
started on this machine:

- the check image and `containers/check.sh` in container mode;
- `Dockerfile.linux` and `scripts/conformance-linux.sh`: gcc 12's warnings under `WERROR=1`, json-c 0.16, glibc's
  `posix_spawn_file_actions_addchdir_np`, `ru_maxrss` in kilobytes (multiplied by 1024 there), and the sanitizers
  on Linux (TSan may need ASLR relaxed, `setarch -R`, on newer kernels).

Start the container (`container system start`) and run `scripts/conformance-linux.sh`, then
`containers/check.sh tests/fixtures/agent-repo`, to close that.
