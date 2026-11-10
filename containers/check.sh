#!/usr/bin/env bash
# Installs a C agent repository and checks what its runner says against the standard, in an Apple
# `container` (Linux, arm64) or, with --native, on this machine. GraniteRust's containers/check.sh, for C.
#
#   containers/check.sh <agent folder | git URL[@revision]> [--native] [--granite <GraniteC clone>]
#                       [--out <dir>] [--keep]
#   (GRANITE_CHECK_MEMORY, 4g, and GRANITE_CHECK_CPUS, 4, size each container)
#
# 1. Install it with GraniteC's installer: granite-agent.json → make O=… <runner> → --describe →
#    installation.json, the record validated against granite-agent-installation.schema.json.
# 2. Describe it: every entry valid against granite-agent-entry.schema.json.
# 3. Every call the agent keeps as a fixture, in its Schema/examples/:
#    *.perform.json  {"perform": {…}, "expect": {"exit": 0}}: answered; the answer valid against Granite's
#                    granite-agent-answer.schema.json and Craft's craft-agent-output.schema.json; every path written;
#                    `made` of kinds it declares; every `expected` made.
#    *.refusal.json  {"perform": {…}, "expect": {"exit": 3, "kind": "call"}}: refused with that kind.
#    The examples folder is the call's folder, so a fixture names its files relative to it.
#
# GraniteC itself comes from this checkout, or from --granite <clone>. --native installs it into a
# scratch prefix and builds the agent against that through PKG_CONFIG_PATH; the container installs it
# into the image. --out keeps each answer, the shelf and the call's folder.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
agent="" mode=container granite="$root" out="" keep=0
while [ $# -gt 0 ]; do
  case $1 in
    --native) mode=native ;;
    --granite) granite=$(cd "$2" && pwd); shift ;;
    --out) out=$2; shift ;;
    --keep) keep=1 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) agent=$1 ;;
  esac
  shift
done
[ -n "$agent" ] || { sed -n '2,22p' "$0"; exit 2; }

scratch=$(mktemp -d "${TMPDIR:-/tmp}/granitec-check.XXXXXX")
[ $keep = 1 ] || [ -n "$out" ] || trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/shelf" "$scratch/work"
say() { printf '\n== %s\n' "$*"; }
failed() { echo "FAILED: $*" >&2; exit 1; }

# The agent, as an installer would fetch it: a clone of its commit when it is a repository of its
# own, else a copy of the folder (one inside another repository, as GraniteC's own fixture is).
if [ -d "$agent" ]; then
  agent_dir=$(cd "$agent" && pwd)
  top=$(git -C "$agent_dir" rev-parse --show-toplevel 2>/dev/null || true)
  if [ "$top" = "$agent_dir" ] && git -C "$agent_dir" rev-parse HEAD >/dev/null 2>&1; then
    [ -z "$(git -C "$agent_dir" status --porcelain)" ] || echo "note: $agent_dir has uncommitted changes; the check installs its last commit" >&2
    git clone --quiet "$agent_dir" "$scratch/agent"
  else
    mkdir -p "$scratch/agent"
    (cd "$agent_dir" && tar --exclude ./build --exclude ./.git -cf - .) | (cd "$scratch/agent" && tar -xf -)
  fi
  examples="$agent_dir/Schema/examples"
  source_in_container=/scratch/agent
  source_native="$scratch/agent"
else
  url=${agent%@*}
  git clone --quiet "$url" "$scratch/agent-read" 2>/dev/null || failed "could not clone $url"
  examples="$scratch/agent-read/Schema/examples"
  source_in_container=$agent
  source_native=$agent
fi
[ -d "$examples" ] && cp -R "$examples/." "$scratch/work/"
commit=$(git -C "$granite" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)

if [ $mode = container ]; then
  container=$(command -v container || ls -d "$HOME"/.local/apple-container/*/bin/container 2>/dev/null | tail -1 || true)
  [ -n "$container" ] || failed "Apple's container is not installed: https://github.com/apple/container/releases"
  "$container" system status >/dev/null 2>&1 || "$container" system start
  say "building the check image"
  "$container" build --tag granitec:check --build-arg GRANITE_COMMIT="$commit" --file "$granite/containers/Dockerfile" "$granite"
  sizing=(--memory "${GRANITE_CHECK_MEMORY:-4g}" --cpus "${GRANITE_CHECK_CPUS:-4}")
  granite_cmd() { "$container" run --rm "${sizing[@]}" --volume "$scratch:/scratch" granitec:check "$@"; }
  inside() { echo "/scratch${1#"$scratch"}"; }
  source=$source_in_container
  schemas=(--schemas /usr/local/share/granitec/schemas)
else
  say "installing GraniteC from $granite into a scratch prefix"
  prefix="$scratch/granitec"
  make -s -C "$granite" O="$scratch/granitec-build" install PREFIX="$prefix" GRANITE_COMMIT="$commit" >/dev/null
  export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
  granite_cmd() { "$prefix/bin/granite" "$@"; }
  inside() { echo "$1"; }
  source=$source_native
  schemas=(--schemas "$prefix/share/granitec/schemas")
fi

say "1. installing $agent ($mode)"
granite_cmd agents add "$source" --shelf "$(inside "$scratch/shelf")" > "$scratch/installation.json"
name=$(jq -r .name "$scratch/installation.json")
runner="$(inside "$scratch/shelf")/$name/agent"
granite_cmd check "${schemas[@]}" --schema granite-agent-installation.schema.json "$(inside "$scratch/shelf/$name/installation.json")"
echo "installed $name: $(jq -r '[.entries[].word] | join(", ")' "$scratch/installation.json") · granite $(jq -r .granite "$scratch/installation.json")"

say "2. describing it"
granite_cmd describe "$runner" > "$scratch/entries.json"
count=$(jq length "$scratch/entries.json")
for index in $(seq 0 $((count - 1))); do
  jq ".[$index]" "$scratch/entries.json" > "$scratch/entry-$index.json"
  granite_cmd check "${schemas[@]}" --schema granite-agent-entry.schema.json "$(inside "$scratch/entry-$index.json")"
done
makes=$(jq -c '[.[].makes // [] | .[].kind]' "$scratch/entries.json")

say "3. calling it"
calls=0
for fixture in "$scratch/work"/*.perform.json "$scratch/work"/*.refusal.json; do
  [ -e "$fixture" ] || continue
  calls=$((calls + 1))
  label=$(basename "$fixture" .json)
  want_exit=$(jq -r '.expect.exit // 0' "$fixture")
  set +e
  granite_cmd run "$runner" "$(inside "$fixture")" --directory "$(inside "$scratch/work")" > "$scratch/$label.line"
  code=$?
  set -e
  [ "$code" = "$want_exit" ] || failed "$label: exit $code, expected $want_exit: $(cat "$scratch/$label.line")"
  if [ "$want_exit" = 0 ]; then
    jq .output "$scratch/$label.line" > "$scratch/$label.answer.json"
    granite_cmd check "${schemas[@]}" --schema granite-agent-answer.schema.json "$(inside "$scratch/$label.answer.json")"
    granite_cmd check "${schemas[@]}" --schema craft-agent-output.schema.json "$(inside "$scratch/$label.answer.json")"
    jq -e '.format == "granite-answer/1" and (.agent.word | type == "string")' "$scratch/$label.answer.json" >/dev/null || failed "$label: not stamped"
    for path in $(jq -r '.paths[]' "$scratch/$label.answer.json"); do
      [ $mode = container ] && path="$scratch${path#/scratch}"
      [ -e "$path" ] || failed "$label: $path is named in paths and is not there"
    done
    jq -e --argjson makes "$makes" '[.made[].kind] - $makes == []' "$scratch/$label.answer.json" >/dev/null || failed "$label: made a kind it does not declare"
    jq -e '([.expected[].name] - [.made[].name]) == []' "$scratch/$label.answer.json" >/dev/null || failed "$label: expected something it did not make"
    echo "$label: answered, $(jq -r '[.made[] | "\(.kind) \(.name) (\(.state))"] | join(", ")' "$scratch/$label.answer.json")"
  else
    want_kind=$(jq -r '.expect.kind' "$fixture")
    kind=$(jq -r '.refused.kind // "none"' "$scratch/$label.line")
    [ "$kind" = "$want_kind" ] || failed "$label: refused as $kind, expected $want_kind: $(cat "$scratch/$label.line")"
    echo "$label: refused ($kind): $(jq -r .failed "$scratch/$label.line")"
  fi
done
[ $calls -gt 0 ] || echo "note: $agent keeps no Schema/examples/*.perform.json or *.refusal.json; nothing was called" >&2

if [ -n "$out" ]; then
  mkdir -p "$out"
  cp -R "$scratch/." "$out/"
  echo "kept in $out"
fi
say "check passed: $name, $calls calls ($mode)"
