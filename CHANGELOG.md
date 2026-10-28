# Changelog

All notable changes to this project are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

## [1.0] — 2026-10-28

First release. [Download](https://github.com/charlesmach/dsi-media-player/releases/tag/v1.0)

### Playback

- FLAC (dr_flac), MP3 (Helix, fixed-point), and WAV decoding
- Streaming engine built on maxmod with a 65536-frame ring buffer (~1.5 s at
  44.1 kHz) to absorb SD card latency spikes
- Linear resampling for sample rates that do not match the output
- Repeat (off / one / all) and shuffle
- Touch-draggable seek bar
- Hardware volume button support via an ARM7 MCU IRQ handler, with an
  on-screen overlay
- Sleep timer — Off / 15 / 30 / 45 / 60 / 90 / 120 minutes

### Library

- SD card tag scan, cached to `/data/dsiplayer_library.dat` so startup does
  not rescan
- Browse by artist / album / track, with an optional album grid view showing
  cover art
- On-screen keyboard search, context-aware by view: artist view matches
  artists, album view matches albums, otherwise track titles
- Multi-artist tags (`"A; B"`) display as the first artist only, while
  still matching under both
- Tag scanning is incremental and driven from the main loop, so audio keeps
  playing while files are indexed (the initial directory enumeration is
  still a blocking walk — see `docs/ARCHITECTURE.md`)

### Cover art

- Embedded art from ID3 `APIC` and FLAC `PICTURE` blocks, plus
  `cover.jpg` / `folder.jpg` / `front.jpg` beside the audio file
- Purpose-built baseline JPEG decoder that scales during the IDCT to emit
  64×64 thumbnails directly, rather than decoding at full resolution
- Thumbnails cached to `/data/artcache/` keyed by source path hash, so album
  grids and hover previews do not re-decode
- Separate playback and preview buffers, so browsing does not clobber the
  now-playing art

### Lyrics

- `.lrc` parsing with timestamp sync, read from beside the audio file
- Fullscreen lyrics view, toggled with X
- Lines too wide for the screen auto-scroll horizontally

### Interface

- Three tabs: Files, Settings, Library
- 512-point fixed-point FFT spectrum visualiser, 16 log-spaced bands with
  attack/decay smoothing and peak hold
- Animated now-playing indicator in file and library lists
- Five selectable accent colour themes
- Metadata preview on hover, with no audio interruption
- Japanese text rendering via an embedded 8×8 bitmap font
- Persistent settings (volume, repeat, shuffle, last directory) and
  resume-on-launch, which restores the previous queue and playback position
  (theme and the album-tiles toggle are not yet persisted)

### Fixed

- **Lyrics and seek bar ran ahead of the audio** by the full ring buffer
  depth. `audioStreamGetPositionMs()` now subtracts the buffer fill level,
  so reported position reflects what is actually audible rather than what
  has been decoded.
- **Visualiser was reading the wrong end of the ring buffer.** Added
  `audioStreamPeekPlayback()` to sample from the read position instead of
  the write position, so the bars match what you hear.
- **FLAC bits-per-sample was misparsed for 32-bit streams.** The expression
  `((si[12] & 1) << 4) | (si[13] >> 4) + 1` binds `+` tighter than `|`,
  yielding 16 instead of 32. Now parenthesised correctly.
- **ROM icon was never applied.** `GAME_ICON` was empty and the committed
  `icon.bmp` was 24-bit, which `ndstool` rejects — it requires a paletted
  bitmap. Added `tools/gen_icon.py` to generate a 32×32 16-colour icon and
  wired it into the build.
- Visualiser bands saturated at full height; magnitude scaling retuned.
- Cover art debug overlay removed from the top screen.
