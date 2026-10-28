# Architecture

How the player is put together, and why it is put together that way. Most of
the non-obvious decisions here exist because of two hard constraints: the
ARM9 has **no FPU**, and audio must never starve.

## Contents

- [Hardware target](#hardware-target)
- [Memory layout](#memory-layout)
- [The main loop](#the-main-loop)
- [Audio pipeline](#audio-pipeline)
- [Decoder interface](#decoder-interface)
- [Playback position](#playback-position)
- [Rendering](#rendering)
- [Music library](#music-library)
- [Cover art](#cover-art)
- [ARM7 side](#arm7-side)
- [Persistence](#persistence)

## Hardware target

Built for DSi-enhanced (TWL) mode. The ROM header carries unit code `0x02`,
so it will not run on an original DS or DS Lite.

| Resource | Original DS (NTR) | DSi (TWL) |
|----------|-------------------|-----------|
| ARM9 clock | 67 MHz | ~134 MHz |
| Main RAM | 4 MB | 16 MB |
| FPU | none | none |
| Screens | 2 × 256×192 | 2 × 256×192 |

The DSi XL has physically larger screens but the same pixel resolution, so UI
elements get bigger rather than more numerous. There is no reason to design
for a denser layout.

**There is no FPU on either model.** Any `float` or `double` arithmetic is
emulated in software by libgcc, which is roughly an order of magnitude slower
than the integer path. This is the single biggest influence on the codebase:
the FFT, the resampler, and the MP3 decoder are all fixed-point for this
reason. Introducing floats into a per-sample or per-pixel path will cause
audible underruns.

## Memory layout

Getting at the DSi's extra RAM takes two coordinated changes, and they must
agree:

| Where | Setting |
|-------|---------|
| `arm9/Makefile` `LDFLAGS` | `-Wl,--defsym,__sp_usr=0x02F00000` |
| `arm9/source/main.c`, top of `main()` | `fake_heap_end = (char *)0x02F00000` |

devkitARM's default linker spec assumes the original DS's 4 MB, which caps
the heap around 3 MB. Moving the user stack top to `0x02F00000` and setting
`fake_heap_end` to match lets `malloc` grow into the DSi's 16 MB instead.

**If you change one, change the other.** A mismatch either wastes most of
main RAM or lets the heap run into the stack, which manifests as corruption
far away from the actual cause.

Largest static allocations, all in `.main.bss` (1,253,960 bytes ≈ 1.2 MB —
check with `arm-none-eabi-size -A arm9/dsi-media-player.elf`):

| Buffer | Size | Where |
|--------|------|-------|
| `Library` struct | 468 KB | `library.c` — 1024 × 464-byte `LibEntry` + view pointers |
| Audio ring buffer | 256 KB | `audio_stream.c` — 65536 stereo frames |
| Album tile cache | 96 KB | `ui_bottom.c` — 12 × 64×64 RGB555 thumbnails |
| Bottom back buffer | 96 KB | `ui_draw.c` — 256×192×2 bytes |
| Scan directory list | 64 KB | `library.c` — 256 × 256-byte paths |
| Cover art (playback + preview) | 16 KB | `cover_art.c` — two 64×64 RGB555 thumbnails |

These are static on purpose. Fragmenting the heap with large long-lived
allocations on a device with no MMU is not worth the flexibility.

## The main loop

`arm9/source/main.c`. One iteration per VBlank (~60 Hz):

```
scanKeys()
  -> touch handling (uiHandleTouch / Held / Release)
  -> uiHandleButtons(kDown, kHeld)
  -> hardware volume overlay if ARM7 reported a change
  -> playerUpdate()

if even frame:                    if odd frame:
  uiTopRender()                     playerUpdate()
  playerUpdate()                    library scan/precache step
  uiBottomRender()                  playerUpdate()
  playerUpdate()                    playerUpdate()

uiSleepTimerTick()
swiWaitForVBlank()
drawSwapBuffers()   (even frames only)
```

Two things here are deliberate and easy to break:

**`playerUpdate()` is called several times per frame, interleaved between
renders.** A full screen render takes long enough that calling decode only
once per frame lets the ring buffer drain. The interleaving keeps the decoder
fed *while* the UI is being drawn. If you add work to the render path, add
another `playerUpdate()` call alongside it.

**The UI renders at 30 fps, not 60.** Rendering happens on even frames only,
which hands roughly half the CPU back to audio decode. Odd frames do
incremental library scanning instead. Lifting this to 60 fps will cause
underruns on FLAC.

Library scanning is mostly stepwise (`libraryScanStep`,
`libraryPreCacheStep`) rather than a blocking loop, so audio keeps playing
while files are being tag-parsed.

**One phase is not stepwise.** `collectDirs()` (`library.c:167`) is called
synchronously from `libraryScanStart()` and walks the entire directory tree
in one go, with a `stat()` per entry. On a full card that blocks the main
loop — and therefore `playerUpdate()` — long enough to drain the ring
buffer. Converting it to a step function is tracked work; until then, the
"audio keeps playing" guarantee holds for the file-scanning phase but not
for the initial directory enumeration.

`SCAN_DIR_MAX` (256 directories) and `LIB_MAX_ENTRIES` (1024 tracks) are
also silent truncation points — a larger collection is quietly cut off
rather than reported.

## Audio pipeline

```
SD card -> libfat -> Decoder -> ring buffer -> maxmod stream -> ARM7 SPU
          (blocking)  (ARM9 main loop)        (MM_TIMER0 callback)
```

`arm9/source/audio/audio_stream.c` owns the ring buffer:

| Constant | Value | Meaning |
|----------|-------|---------|
| `RING_SIZE` | 65536 | stereo frames (~1.5 s at 44.1 kHz) |
| `DECODE_CHUNK` | 1024 | frames decoded per iteration |
| `STREAM_BUF_LEN` | 2048 | maxmod stream buffer length |

The ring is a power of two, so wraparound is a mask (`RING_MASK`) rather
than a modulo — relevant because `ringAvailable()` is called on the audio
callback path.

`ringWritePos` / `ringReadPos` are `volatile` and are written by different
contexts: the main loop writes, the maxmod timer callback reads. They are
free-running counters masked on use, which makes a full ring
distinguishable from an empty one without a separate flag.

The ~1.5 s of slack exists to absorb SD card latency spikes. A single slow
read on a fragmented card can stall for tens of milliseconds, which is
plenty to produce a click with a smaller buffer.

A linear resampler handles files whose sample rate does not match the
output rate.

## Decoder interface

`arm9/source/audio/decoder.h`. Each decoder fills in a vtable:

```c
struct Decoder {
    AudioFormat format;
    AudioInfo   info;
    AudioMeta   meta;
    void       *ctx;     /* decoder-private state */
    FILE       *file;

    DecodeResult (*open)(Decoder *dec, const char *path);
    DecodeResult (*readSamples)(Decoder *dec, s16 *outBuf,
                                u32 numSamples, u32 *samplesRead);
    DecodeResult (*seek)(Decoder *dec, u32 samplePos);
    u32          (*tell)(Decoder *dec);
    void         (*close)(Decoder *dec);
};
```

Contract notes:

- `readSamples` returns **interleaved stereo `s16`**. Mono sources are
  duplicated to both channels by the decoder, so everything downstream can
  assume stereo.
- `numSamples` counts **frames**, not individual `s16` values. A buffer for
  `n` frames holds `2n` shorts.
- A short read is normal. Set `*samplesRead` and return `DECODE_OK`; return
  `DECODE_EOF` only when the stream is genuinely finished.
- `seek` and `tell` work in frame positions from the start of the stream.
- `close` must release everything in `ctx`; `decoderCleanup` calls it.

`detectFormat()` dispatches on **file extension**, not content — a `.mp3`
that is actually FLAC will be handed to the wrong decoder and fail to open.
`decoderInit()` then zeroes the struct, calls the matching
`decoderInitXxx()` to install the vtable, and calls `open()`, cleaning up
if it fails.

`decoderParseMeta()` reads tags and locates embedded cover art **without**
instantiating a decoder — the library scanner and the metadata preview rely
on that being cheap.

See [ADDING_A_DECODER.md](ADDING_A_DECODER.md) for a walkthrough.

### Why Helix for MP3

The obvious choice is minimp3 (CC0, single header), and the repo history
started there. It decodes to float internally, which on an FPU-less ARM9
means software emulation per sample, and it could not reliably keep the ring
buffer fed. Helix is fixed-point and does. The tradeoff is licensing: Helix
is RCSL/RPSL, so it is vendored under `libs/helix/` and kept separate from
the project's MIT code.

## Playback position

Worth understanding before touching anything time-related.

`audioStreamGetPosition()` returns the **decoded** position — how far the
decoder has read. That is ahead of what the user is hearing by exactly the
amount sitting in the ring buffer, up to ~1.5 s.

`audioStreamGetPositionMs()` corrects for this:

```c
u32 buffered = ringAvailable();
pos = (pos > buffered) ? pos - buffered : 0;
return (u32)((u64)pos * 1000 / rate);
```

Everything user-facing — the seek bar, the elapsed time, lyric line
selection — must use the corrected value. Using the raw decoded position
makes lyrics run ahead of the music by the full buffer depth, which is
very visible.

Note the `(u64)` cast. `pos * 1000` overflows 32 bits after about 97
seconds at 44.1 kHz.

The visualiser has the same problem and solves it differently:
`audioStreamPeekPlayback()` reads from `ringReadPos` (what is playing now)
rather than `ringWritePos` (what was just decoded), so the bars match what
you hear.

## Rendering

Both screens are 16-bit RGB555 framebuffers, but they are set up
differently (`ui_draw.c`):

**Top screen** — true double buffering. VRAM banks A and B hold two pages;
`drawSwapBuffers()` flips which one the background layer points at via
`bgSetMapBase()`. No copy.

**Bottom screen** — a single VRAM bank (C) plus a 96 KB back buffer in main
RAM. `drawSwapBuffers()` `dmaCopy`s the back buffer into VRAM. The extra
copy is the cost of the sub engine having less VRAM to spare.

Drawing primitives live in `ui_draw.c`. Two string families:

- `drawString*` — ASCII, 6×8 font, from `font_data.h`
- `drawStringU*` — UTF-8 aware, falls back to the 8×8 Japanese font in
  `font_jp_data.h` for non-ASCII

Both font headers are generated, not hand-written; `tools/gen_jpfont.py`
rasterizes a system TrueType face to bitmaps. There are no external asset
files at runtime.

Per-pixel bounds checks in the glyph blitters mean negative `x` is safe,
which is what makes the marquee and lyric auto-scroll work — they just draw
at a negative offset and let clipping handle it.

### Visualiser

`ui_top.c`. A 512-point radix-2 DIT FFT in Q14 fixed point, with a Hann
window, feeding 16 log-spaced bands with attack/decay smoothing and peak
hold. The tables (`fftSinQ`, `fftWindow`, `bandEdge`) are hand-written
literals in `ui_top.c` — there is **no generator script**, so changing the
FFT size currently means regenerating 512 Hann coefficients by hand. Adding
a `tools/gen_fft_tables.py` is tracked work.

`re` and `im` are right-shifted (`>> 7`) **before squaring**, which sets the
noise floor; the log divisor (`/ 14`) sets how many octaves map to full bar
height. The two are a coupled pair — change one and you must retune the
other.

Two known defects in this path:

- **The butterfly has no per-stage scaling** (`ui_top.c:403-409`), so
  magnitudes can grow by ~2⁹ across the nine stages. `re * re + im * im` is
  then computed in `s32`, which can overflow on loud transients and wrap a
  band to near-zero.
- `isqrt32()` runs eight Newton iterations, each with a *software* 32-bit
  divide, ~1850 times per FFT. The DS has a **hardware square root unit** —
  libnds exposes it as `sqrt32()` in `nds/arm9/math.h`.

## Music library

`arm9/source/library.c`. Flat array of up to `LIB_MAX_ENTRIES` (1024)
`LibEntry` records, each holding path plus pre-parsed tags.

Scanning is incremental, driven one step per odd frame from the main loop,
and the result is cached to `/data/dsiplayer_library.dat` so startup does
not rescan.

Navigation is a view projection over the flat array rather than a tree.
`libraryBuildView()` rebuilds `viewList` — an array of `char *` pointing
into the entries — based on `view` (`ARTISTS` / `ALBUMS` / `TRACKS`) plus
`filterArtist` / `filterAlbum`. `libraryEnter()` descends by setting a
filter and rebuilding.

Two behaviours that are easy to regress:

- **Artist display takes the first artist only.** Multi-artist tags
  (`"A; B"`) show as `A`, rather than splitting into separate entries.
  Matching still uses `artistContains()`, so a track tagged `"A; B"` is
  found under both.
- **Search is view-aware.** In artist view it matches artist names and
  returns unique first-artists; in album view it matches albums; otherwise
  it matches track titles. `viewList` entries must stay valid for the
  current view — in artist and search-artist views they point at `static`
  dedup buffers, not into `entries`.

## Cover art

`cover_art.c` plus `jpegthumb.c`.

Sources, in order: embedded art (ID3 `APIC` / FLAC `PICTURE`), then
`cover.jpg` / `folder.jpg` / `front.jpg` beside the audio file.

`jpegthumb.c` is a purpose-built baseline JPEG decoder that **scales during
the IDCT** to emit a 64×64 thumbnail directly, instead of decoding at full
resolution and downsampling. A 3000×3000 album scan would not fit
comfortably otherwise.

Only baseline JPEG is supported. Progressive is rejected rather than
mis-decoded.

Thumbnails are cached to `/data/artcache/<hash>.thm` — an 8 KB file holding
a `"THUB"` magic plus 64×64 RGB555 — keyed by a DJB2-ish hash of the source
path. Album grids and hover previews read the cache, so scrolling a grid
does not re-decode anything.

Playback art and preview art are separate buffers, selected by
`coverArtSetPreviewMode()`, so browsing while a track plays does not
clobber the now-playing art.

## ARM7 side

`arm7/source/main.c`, 84 lines. Default sound/touch handling plus one
custom job: the hardware volume buttons.

The MCU does not update its volume register unless its volume-button IRQ is
enabled, so the ARM7 calls `mcuIrqSet(MCU_IRQ_VOLBTN, ...)`, reads
`McuReg_VolumeLevel` over I2C when the flag is set, and forwards the value
(0–31) to the ARM9 over PXI channel `User0`. The ARM9 picks it up in its PXI
handler and raises `hwVolumeChanged`, which the main loop turns into the
on-screen overlay.

Without that `mcuIrqSet` call the volume buttons appear to do nothing.

It also handles the lid. `pmSetSleepAllowed(false)` disables the automatic
sleep that would otherwise halt playback when the hinge closes, and on each
hinge transition it toggles `PMIC_CTRL_LCD_BL_TOP` / `_BOTTOM` through the
PMIC to kill both backlights. That combination is what makes the device
usable as a pocket player with the lid shut.

## Persistence

| Path | Contents |
|------|----------|
| `/data/dsiplayer.cfg` | volume, repeat, shuffle, last path/directory, resume position |
| `/data/dsiplayer_resume.m3u` | queue snapshot for resume-on-launch |
| `/data/dsiplayer_library.dat` | cached library index |
| `/data/artcache/*.thm` | 64×64 cover art thumbnails |

Settings are written periodically from the main loop
(`SETTINGS_SAVE_INTERVAL`, every 300 frames) rather than on every change, to
avoid hammering the SD card. Power loss can therefore lose the last few
seconds of preference changes, which is an accepted tradeoff.

**Not currently persisted:** the selected theme and the album-tiles toggle
are file-statics in `ui_bottom.c` (`currentTheme`, `showAlbumTiles`) with no
corresponding field in `Settings`, so both reset on every launch. Adding
them is a small, self-contained change.
