# devkitA64. Phase 6.
#
# Nothing extra is needed here: an NRO comes out of the toolchain, and the Switch
# has no equivalent of the CIA permission problem that makes the 3DS build depend
# on third party tools.
FROM devkitpro/devkita64:latest

WORKDIR /work
