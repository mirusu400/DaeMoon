# The desktop side: core tests, the server, daemoonctl, and the end to end run.
#
# The Go version tracks server/go.mod. They were allowed to drift once - the image
# said 1.22 while the module said 1.25 - and everything that used the image failed on
# a line nobody was reading.
#
# Everything here also works on a plain machine with gcc, clang and Go installed.
# The image exists so a contributor gets the same compilers and the same sanitizer
# behaviour as CI without a shopping list.
FROM golang:1.25-bookworm

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        build-essential clang make git ca-certificates; \
    rm -rf /var/lib/apt/lists/*

# Go writes its caches under HOME, and the container runs as the invoking user, so
# point them somewhere writable regardless of who that is.
ENV GOCACHE=/tmp/go-build GOMODCACHE=/tmp/go-mod GOFLAGS=-buildvcs=false

WORKDIR /work
