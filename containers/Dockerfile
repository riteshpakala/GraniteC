# syntax=docker/dockerfile:1
# The check image: a C toolchain, pkg-config, json-c, git and GraniteC installed, so a container can
# install any C agent repository with GraniteC's installer (granite-agent.json → make → --describe →
# installation.json) and drive its runner as a host would. containers/check.sh builds and runs it with
# Apple's `container`.
FROM docker.io/library/debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential pkg-config libjson-c-dev git ca-certificates jq \
 && rm -rf /var/lib/apt/lists/*

# The commit GraniteC was built from: .git is not in the build context, so check.sh passes it.
ARG GRANITE_COMMIT=unknown
WORKDIR /src/GraniteC
COPY . .
RUN make install PREFIX=/usr/local GRANITE_COMMIT="$GRANITE_COMMIT" \
 && mkdir -p /schemas \
 && cp Schema/*.schema.json containers/schemas/*.schema.json /schemas/ \
 && make clean

WORKDIR /scratch
ENTRYPOINT ["granite"]
