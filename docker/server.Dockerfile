# The server, as one image.
#
# "One static binary plus one database file" is the whole deployment promise, and a
# container should not make that less true. So the runtime stage is `scratch`: the
# binary, a passwd file, and nothing else. There is no shell in here, which also means
# there is no shell for anything that gets in to use.
#
# The SQLite driver is pure Go (modernc), so CGO_ENABLED=0 is not a compromise to make
# this work - it is what the database choice was made for.
# --platform=$BUILDPLATFORM: the build stage always runs natively and Go cross compiles
# to the target. Emulating an arm64 toolchain under QEMU to produce an arm64 binary is
# slower and it does not work - `go mod download` dies with exit 255 in the emulator.
# The compiler already knows how to target another architecture; let it.
FROM --platform=$BUILDPLATFORM golang:1.25-bookworm AS build

WORKDIR /src

# The module first, so a change to a source file does not re-download the module cache.
COPY server/go.mod server/go.sum ./server/
RUN cd server && go mod download

COPY server ./server
COPY shared ./shared

ARG VERSION=dev
# Set by buildx. Defaulted so a plain `docker build` still works.
ARG TARGETOS=linux
ARG TARGETARCH
ARG TARGETVARIANT
RUN set -eux; \
    cd server; \
    export GOOS="${TARGETOS}" GOARCH="${TARGETARCH}"; \
    case "${TARGETVARIANT}" in v7) export GOARM=7 ;; v6) export GOARM=6 ;; esac; \
    CGO_ENABLED=0 go build -trimpath \
      -ldflags "-s -w -X main.version=${VERSION}" \
      -o /out/daemoond ./cmd/daemoond; \
    file /out/daemoond || true

# A user to run as, built here because scratch has no adduser. 65532 is the
# conventional unprivileged id and matches what distroless uses, so a volume created by
# one is writable by the other.
RUN printf 'daemoon:x:65532:65532:daemoon:/nonexistent:/sbin/nologin\n' > /out/passwd

# /tmp, because scratch has no directories at all and an upload is staged in one.
#
# A zip has to be read back to front to find its central directory, so a package is
# written to a temp file before it is verified rather than buffered in memory. Without
# this the image builds, starts, serves /healthz, signs people in - and fails every
# single upload with internal_error. Found by uploading to it rather than by reading
# the Dockerfile.
RUN mkdir -m 1777 /out/tmp

FROM scratch

COPY --from=build /out/passwd /etc/passwd
COPY --from=build /out/daemoond /daemoond
COPY --from=build --chown=65532:65532 /out/tmp /tmp

# Where the one database file goes. Everything the service knows is in here - accounts,
# devices, metadata and every save blob - which is the point of putting blobs in SQLite
# rather than on a filesystem: this directory is the backup.
VOLUME ["/data"]
ENV DAEMOON_DB=/data/daemoon.db \
    DAEMOON_ADDR=:8080

USER 65532:65532
EXPOSE 8080

# No HEALTHCHECK instruction: it would need a shell or curl, and neither is in here.
# The endpoint exists - /healthz - and an orchestrator that wants to poll it can, which
# is what compose.yml does.
ENTRYPOINT ["/daemoond"]
