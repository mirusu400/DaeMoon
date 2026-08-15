# devkitARM plus the two third party tools a CIA needs.
#
# The CIA is the point. A .3dsx cannot reach another title's save archive, so the
# permissions in app.rsf are what make this project work at all, and makerom is
# what applies them. A .3dsx build is still useful for iterating on the UI, and it
# needs nothing beyond the base image.
FROM devkitpro/devkitarm:latest

# Neither tool is in the devkitPro repositories.
#
# makerom is upstream. bannertool's original repository (Steveice10) was deleted,
# so this uses the maintained fork; the binary is identical in purpose and is
# pinned by checksum, which is what actually matters when a build tool disappears
# and reappears elsewhere.
#
# Both are verified rather than trusted. A build tool that silently changes is a
# build that silently changes, and this one signs the permissions that let the app
# reach other titles' save data.
# ctrtool reads a CIA back. It is what turns "the permissions in app.rsf are
# probably right" into something checkable without a console: the exheader either
# carries the filesystem rights and the service list, or it does not.
ARG CTRTOOL_VERSION=1.3.0
ARG CTRTOOL_SHA256=f553194b0ab2b539457160231cf5651c554e148f17ec960fae63424e82582aa8
ARG MAKEROM_VERSION=0.18.4
ARG MAKEROM_SHA256=dd596854718c195c6e3229286be485b122921715555af8ae5cf8e9a465d9f970
ARG BANNERTOOL_VERSION=v1.2.2
ARG BANNERTOOL_SHA256=e4259c08fe8944ebadd5f4b96f9a8603e5427338074cfc46323fbbc3410d51ed

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends unzip ca-certificates curl; \
    rm -rf /var/lib/apt/lists/*; \
    \
    curl -fsSL -o /tmp/makerom.zip \
      "https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v${MAKEROM_VERSION}/makerom-v${MAKEROM_VERSION}-ubuntu_x86_64.zip"; \
    echo "${MAKEROM_SHA256}  /tmp/makerom.zip" | sha256sum -c -; \
    unzip -j -o /tmp/makerom.zip makerom -d /opt/devkitpro/tools/bin; \
    \
    curl -fsSL -o /tmp/bannertool.zip \
      "https://github.com/Epicpkmn11/bannertool/releases/download/${BANNERTOOL_VERSION}/bannertool.zip"; \
    echo "${BANNERTOOL_SHA256}  /tmp/bannertool.zip" | sha256sum -c -; \
    unzip -j -o /tmp/bannertool.zip linux-x86_64/bannertool -d /opt/devkitpro/tools/bin; \
    \
    curl -fsSL -o /tmp/ctrtool.zip \
      "https://github.com/3DSGuy/Project_CTR/releases/download/ctrtool-v${CTRTOOL_VERSION}/ctrtool-v${CTRTOOL_VERSION}-ubuntu_x86_64.zip"; \
    echo "${CTRTOOL_SHA256}  /tmp/ctrtool.zip" | sha256sum -c -; \
    unzip -j -o /tmp/ctrtool.zip ctrtool -d /opt/devkitpro/tools/bin; \
    \
    chmod +x /opt/devkitpro/tools/bin/makerom /opt/devkitpro/tools/bin/bannertool \
             /opt/devkitpro/tools/bin/ctrtool; \
    rm -f /tmp/makerom.zip /tmp/bannertool.zip /tmp/ctrtool.zip

WORKDIR /work
