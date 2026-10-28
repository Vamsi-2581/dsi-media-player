# Third-Party Libraries

## Fetched by `setup.sh` (not committed)

### dr_flac.h — FLAC decoder
- **Source**: https://github.com/mackron/dr_libs
- **License**: Public domain (Unlicense) or MIT-0, at your choice
- **Notes**: Integer arithmetic throughout, which matters on the ARM9's
  FPU-less core.

## Vendored in this repository

### helix/ — Helix fixed-point MP3 decoder
- **Source**: Helix DNA, RealNetworks (helixcommunity.org)
- **License**: RCSL 1.0 / RPSL 1.0 — each source file carries the full
  license block in its header. These terms govern the `libs/helix/`
  subtree and are **not** covered by this project's MIT license.
- **Notes**: Chosen over minimp3 because it is integer-only. minimp3 uses
  floats internally, which the ARM9 has to emulate in software.

## Not used

Ogg Vorbis is stubbed out in `arm9/source/audio/decoder_vorbis.c`.
Implementing it would mean building [Tremor](https://gitlab.xiph.org/xiph/tremor)
(BSD, integer-only) as a static library for ARM9 and adding it to `LIBS`
in `arm9/Makefile`.
