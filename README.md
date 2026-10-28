# DSi Media Player

A homebrew audio player for the Nintendo DSi (XL), built for DSi-enhanced
(TWL) mode so it can use the hardware the original DS doesn't have:

- **ARM9 @ 134 MHz** (vs 67 MHz on original DS)
- **16 MB main RAM** (vs 4 MB)
- **SD card storage** via libfat

Produces a single `.nds` you copy to the SD card and launch from
TWiLight Menu++ or any homebrew launcher.

## Features

- **Formats**: FLAC, MP3, WAV
- **Music library** — scans the SD card for tags and browses by
  artist / album / track, with an optional album grid view showing cover art
- **Search** — on-screen keyboard, context-aware: searching in artist view
  matches artists, in album view matches albums, otherwise matches tracks
- **Cover art** — embedded (ID3 / FLAC picture blocks) or `cover.jpg` /
  `folder.jpg` alongside the audio, decoded to 64×64 thumbnails and cached
  to disk so repeat loads are instant
- **Synced lyrics** — reads `.lrc` files next to the audio file, with a
  fullscreen view; long lines auto-scroll
- **Spectrum visualiser** — 512-point fixed-point FFT, 16 log-spaced bands
  with peak hold
- **Themes** — five accent colours applied across both screens
- **Sleep timer** — Off / 15 / 30 / 45 / 60 / 90 / 120 minutes
- **Gapless-ish playback**, repeat (off / one / all), shuffle, and a
  touch-draggable seek bar
- **Plays with the lid closed** — automatic sleep is disabled and both
  backlights switch off on hinge close, so it works as a pocket player
- Persistent settings and resume-on-launch, restoring your queue and
  playback position
- Audio keeps playing while the library tag-scans

## Supported Formats

| Format | Decoder  | License       | Status |
|--------|----------|---------------|--------|
| FLAC   | dr_flac  | Public domain / MIT-0 | 16-bit 44.1–48 kHz solid; 24-bit / 96 kHz best-effort |
| WAV    | built-in | —             | All standard PCM configurations |
| MP3    | Helix    | RCSL / RPSL   | All bitrates (32–320 kbps), integer-only |
| Ogg    | stub     | —             | Not implemented |

MP3 uses the Helix fixed-point decoder rather than a float one, because the
ARM9 has no FPU and software float emulation is too slow to keep the ring
buffer fed.

## Documentation

| Document | What it covers |
|----------|----------------|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How it works — memory layout, main loop, audio pipeline, rendering, library, cover art |
| [docs/ADDING_A_DECODER.md](docs/ADDING_A_DECODER.md) | Walkthrough for wiring in a new audio format |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Dev setup, code style, build gotchas, testing, PR process |
| [CHANGELOG.md](CHANGELOG.md) | Version history |
| [FEASIBILITY.md](FEASIBILITY.md) | Original feasibility study — hardware limits that shaped the design |

## Building

### Prerequisites

- [devkitPro](https://devkitpro.org/) with devkitARM
- libnds, libfat, maxmod — `dkp-pacman -S nds-dev`
- `DEVKITPRO` and `DEVKITARM` set in your environment

### Build

```bash
./setup.sh && make
```

`setup.sh` downloads `dr_flac.h` (not vendored) and generates `icon.bmp`.
The Helix MP3 decoder is vendored under `libs/helix/`.

> **Note**: paths containing an apostrophe break the devkitARM build rules.
> If your checkout is under such a path, build from a copy elsewhere.

## Controls

| Button | Action |
|--------|--------|
| D-Pad Up/Down | Navigate list |
| D-Pad Left/Right | Page up / down; move within album tiles; adjust setting |
| A | Enter directory or album / play track / activate setting |
| B | Go up one level |
| X | Toggle top screen: now playing ↔ lyrics |
| Y | Toggle search (library tab) |
| START | Play / Pause |
| SELECT | Cycle tabs: Files / Settings / Library |
| L | Previous track |
| R | Next track |
| Volume buttons | Hardware volume, with an on-screen overlay |

### Touch (bottom screen)

- Tap the tab bar to switch between Files, Settings, and Library
- Tap a list item or album tile to select, tap again to play
- Transport controls: `|<<` `>||` `>>|` and the repeat / shuffle toggles
- Tap or drag the seek bar to scrub
- On-screen keyboard when search is active

## Project Structure

```
arm9/source/
  main.c                  Entry point, main loop, input dispatch
  player.c/h              Player state machine, playlist
  library.c/h             SD card tag scan, artist/album/track views, search
  settings.c/h            Persistent settings
  audio/
    audio_stream.c/h      Streaming engine (maxmod + ring buffer)
    decoder.h             Abstract decoder interface
    decoder_common.c      Format detection, tag + cover art parsing
    decoder_wav.c         WAV/PCM
    decoder_flac.c        FLAC via dr_flac
    decoder_mp3.c         MP3 via Helix
    decoder_vorbis.c      Ogg Vorbis stub
    helix_mp3.c           Helix decoder translation unit
  ui/
    ui.h                  UI types, colours, drawing API
    ui_draw.c             Framebuffer primitives, 6x8 + JP font rendering
    ui_top.c              Top screen: now playing, FFT visualiser, lyrics
    ui_bottom.c           Bottom screen: tabs, lists, touch, transport
    cover_art.c/h         Cover art extraction + thumbnail disk cache
    jpegthumb.c/h         Baseline JPEG decoder producing 64x64 thumbnails
    lyrics.c/h            .lrc parsing and rendering
    font_data.h           Embedded 6x8 bitmap font
    font_jp_data.h        Embedded Japanese font
  fs/
    file_browser.c/h      Directory listing, navigation, filtering

arm7/source/main.c        ARM7 (sound, touch, hardware volume reporting)

libs/helix/               Vendored Helix MP3 decoder (RCSL/RPSL)
tools/                    Font + icon generators, host-side JPEG test
```

## Architecture Notes

### Audio pipeline

```
SD card -> libfat -> decoder -> ring buffer -> maxmod stream -> ARM7 SPU
```

Decoding runs on the ARM9 in the main loop; maxmod's timer callback pulls
from a 65536-sample stereo ring buffer (~1.5 s at 44.1 kHz), which absorbs
SD card latency spikes. A linear resampler handles mismatched sample rates.

Playback position is reported as the decoded position *minus* the ring
buffer fill level — without that correction, lyrics and the seek bar run
ahead of what you actually hear by the full buffer depth.

### UI rendering

Both screens use 16-bit RGB555 framebuffers (BG2 bitmap on each engine).
Fonts are embedded as bitmap data, so there are no external asset files.
The Japanese font header is generated by `tools/gen_jpfont.py`, which
rasterizes a macOS system font to 8×8 bitmaps at build-prep time.

### Cover art

JPEG decoding uses a purpose-built baseline decoder (`jpegthumb.c`) that
scales during the IDCT to emit a 64×64 thumbnail directly, rather than
decoding full resolution and downsampling. Thumbnails are cached to
`/data/artcache/` keyed by a hash of the source path, so album grids and
previews don't re-decode.

## Limitations

- **Ogg Vorbis** is stubbed; it needs Tremor built for ARM9.
- **MP3 seeking** scans frame-by-frame (no seek table), so seeking far into
  a long MP3 takes a moment.
- **Progressive JPEG** cover art is not supported — baseline only.

## License

Project code is MIT (see [LICENSE](LICENSE)).

Third-party components keep their own licenses:

- `libs/helix/` — Helix MP3 decoder, RCSL 1.0 / RPSL 1.0 (RealNetworks).
  Each file carries its full license block. **Not** covered by this
  project's MIT license.
- `dr_flac.h` — public domain (Unlicense) or MIT-0, fetched by `setup.sh`.

See [libs/README.md](libs/README.md) for details.
