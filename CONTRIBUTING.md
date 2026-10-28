# Contributing

Thanks for taking a look. This is a homebrew project for real hardware, so
the main thing worth knowing up front is that **the compiler passing is not
the same as it working** — see [Testing](#testing).

## Getting set up

### Prerequisites

Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with
devkitARM, then the DS toolchain:

```bash
dkp-pacman -S nds-dev
```

You need `DEVKITPRO` and `DEVKITARM` exported. On a default macOS/Linux
install:

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
```

### Build

```bash
./setup.sh   # fetches dr_flac.h, generates icon.bmp
make
```

That produces `dsi-media-player.nds` in the repo root.

`setup.sh` is only needed once. `dr_flac.h` is deliberately not committed —
it is a third-party single-header library.

### Known build gotchas

**Apostrophes in the path break the build.** devkitARM's `ds_rules` does not
quote paths properly, so a checkout under something like
`/Users/you/Albert's Mac/` fails with confusing errors. Build from a path
without an apostrophe.

**`make` derives the ROM name from the directory name.** `Makefile` has
`TARGET := $(shell basename $(CURDIR))`, so building in a directory called
`foo` produces `foo.nds` with `foo` baked into the ROM internals. If you are
producing a release artifact, build in a directory named
`dsi-media-player` so the output matches what everyone else gets.

**`make clean` does not remove the `.nds`** from a previous differently-named
build. Clean stale ones by hand if you renamed the directory.

## Project layout

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how it all fits
together, and the README for the file-by-file tree.

Quick orientation:

| I want to change... | Start in |
|---------------------|----------|
| Audio decoding | `arm9/source/audio/` |
| Add a format | `audio/decoder.h` + [docs/ADDING_A_DECODER.md](docs/ADDING_A_DECODER.md) |
| Now-playing screen, visualiser, lyrics view | `arm9/source/ui/ui_top.c` |
| Tabs, lists, touch, transport controls | `arm9/source/ui/ui_bottom.c` |
| Drawing primitives, fonts | `arm9/source/ui/ui_draw.c` |
| Library scanning, views, search | `arm9/source/library.c` |
| Cover art, thumbnails | `arm9/source/ui/cover_art.c`, `jpegthumb.c` |
| Input dispatch, main loop | `arm9/source/main.c` |
| Hardware volume, SPU, touch | `arm7/source/main.c` |

## Code style

Matching the surrounding code matters more than any rule here.

- 4 spaces, no tabs. Opening brace on the same line.
- `lowerCamelCase` for functions and variables, `UpperCamelCase` for types,
  `SCREAMING_SNAKE` for macros.
- Module prefixes on public functions: `playerFoo`, `uiFoo`, `libraryFoo`,
  `coverArtFoo`, `audioStreamFoo`. Anything not in a header should be
  `static`.
- C99. The build is `-Wall` at `-O3`; **do not add new warnings.** Warnings
  from `libs/helix/` are pre-existing vendored code — leave them.
- Comments explain *why*, not *what*. Most of this codebase has non-obvious
  constraints behind it; that is what is worth writing down.

### Things that will cause real bugs

**No floating point in per-sample or per-pixel paths.** There is no FPU.
`float` works but is software-emulated and roughly an order of magnitude
slower than integer. The FFT, resampler, and MP3 decoder are all fixed-point
for this reason. A `float` in a hot loop will cause audible underruns.

**Watch for 32-bit overflow in time arithmetic.** `samples * 1000` overflows
after ~97 seconds at 44.1 kHz. Cast to `u64` first — see
`audioStreamGetPositionMs()`.

**Feed the decoder if you add work to the render path.** The main loop
interleaves `playerUpdate()` between render calls on purpose. Adding a slow
render step without a `playerUpdate()` next to it will starve audio.

**Use `audioStreamGetPositionMs()`, not `audioStreamGetPosition()`**, for
anything the user sees. The raw value is the *decoded* position, up to ~1.5 s
ahead of what is actually audible.

**Keep `__sp_usr` and `fake_heap_end` in agreement.** They are set in
`arm9/Makefile` and `arm9/source/main.c` respectively, both to
`0x02F00000`. A mismatch silently wastes most of main RAM or corrupts memory.

## Testing

There is no unit test suite. Verification is manual, and the honest state of
things is:

1. **It must compile with no new warnings.** Necessary, nowhere near
   sufficient.
2. **Test in an emulator.** [melonDS](https://melonds.kuribo64.net/) is the
   most accurate for DSi mode; set up an SD card image with some music.
   Catches crashes and logic errors.
3. **Test on real hardware for anything audio-related.** Emulators do not
   reproduce SD card latency or the real CPU timing, which is exactly where
   underruns come from. If you touch the decoders, the ring buffer, or the
   main loop timing, an emulator pass means very little.

If you cannot test on hardware, say so in the PR. That is useful
information, not a disqualification.

Worth exercising when changing playback: FLAC and MP3 of different sample
rates, a track with embedded art, a track with `cover.jpg` beside it, a
track with neither, a `.lrc` with long lines, and seeking far into a long
MP3.

`tools/test_jpeg.c` is a host-side harness for the thumbnail decoder — it
compiles `jpegthumb.c` natively so you can test JPEG changes without
deploying:

```bash
cc -o /tmp/test_jpeg tools/test_jpeg.c && /tmp/test_jpeg some.jpg
```

## Submitting changes

1. Fork and branch off `main`.
2. Keep commits focused; commit messages should say *why*.
3. Open a PR describing what changed, why, and **how you tested it**
   (emulator, hardware, or neither).

Small, single-purpose PRs get reviewed faster than large ones. If you are
planning something substantial — a new decoder, reworking the UI — open an
issue first so we can talk about the approach before you spend the time.

## Licensing

Project code is MIT. By contributing you agree your changes ship under it.

`libs/helix/` is RealNetworks' Helix MP3 decoder under RCSL 1.0 / RPSL 1.0.
It is **not** MIT and must stay separately licensed — do not move code
between `libs/helix/` and the project tree in either direction, and do not
strip the license blocks from those files.

## Good first contributions

- **Ogg Vorbis** — `decoder_vorbis.c` is stubbed. Needs
  [Tremor](https://gitlab.xiph.org/xiph/tremor) (BSD, integer-only) built
  for ARM9.
- **MP3 seek table** — seeking currently scans frame-by-frame.
- **Progressive JPEG** in `jpegthumb.c`, or graceful fallback art.
- **`strncpy` truncation warnings** — ~20 sites where truncation is
  intentional and the code null-terminates afterward, but GCC cannot see
  that. A small `safeCopy` helper would make the intent explicit and quiet
  the build.
- **Playlist save/load UI** — `playerPlaylistSave`/`Load` exist but are not
  exposed in the interface.
