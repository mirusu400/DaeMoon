# ctrtool, for reading a built CIA back.
#
# Its own image because the release binary needs a newer glibc than the devkitPro
# image has, and because the alternative - trusting that makerom did what app.rsf
# said - is the thing this exists to stop doing.
FROM ubuntu:24.04

ARG CTRTOOL_VERSION=1.2.1
ARG CTRTOOL_SHA256=34a1d357dea320efa33babe8c20523a18e8f7287b9a74050e67459e2eaf48f77

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends unzip ca-certificates curl; \
    rm -rf /var/lib/apt/lists/*; \
    curl -fsSL -o /tmp/ctrtool.zip \
      "https://github.com/3DSGuy/Project_CTR/releases/download/ctrtool-v${CTRTOOL_VERSION}/ctrtool-v${CTRTOOL_VERSION}-ubuntu_x86_64.zip"; \
    echo "${CTRTOOL_SHA256}  /tmp/ctrtool.zip" | sha256sum -c -; \
    unzip -j -o /tmp/ctrtool.zip ctrtool -d /usr/local/bin; \
    chmod +x /usr/local/bin/ctrtool; \
    rm -f /tmp/ctrtool.zip

WORKDIR /work
