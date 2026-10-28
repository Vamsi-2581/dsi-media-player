# DSi Media Player — Feasibility Summary

## Hardware Profile (DSi-Enhanced / TWL Mode)

| Resource           | Original DS (NTR) | DSi Enhanced (TWL) | Notes                              |
|--------------------|-------------------|--------------------|------------------------------------|
| ARM9 clock         | 67 MHz            | ~133 MHz           | 2x decode headroom                 |
| Main RAM           | 4 MB              | 16 MB              | Huge buffers, album art, playlists |
| Storage            | Slot-1 flashcart  | SD card (SDHC)     | libfat via `fat:` or `sd:` prefix  |
| Sound hardware     | 16 channels       | 16 channels        | Same SPU; no new DAC exposed       |
| Screen resolution  | 256x192 x2        | 256x192 x2         | Unchanged; XL has larger physical  |
| FPU                | None              | None               | Integer decoders mandatory         |

## Audio Format Feasibility

### 1. FLAC — PRIMARY TARGET

Decoder: **dr_flac** (public domain, single-header, pure integer)

| Configuration       | Real-time? | Notes                                     |
|---------------------|------------|-------------------------------------------|
| 16-bit / 44.1 kHz   | YES        | Solid, no dropouts expected               |
| 16-bit / 48 kHz     | YES        | Comfortable margin                        |
| 24-bit / 44.1 kHz   | YES        | dr_flac handles natively, output truncated to 16-bit for SPU |
| 24-bit / 96 kHz     | BEST-EFFORT | 4x data rate of CD; decoding likely OK but SD I/O throughput is the bottleneck — may underrun on large block sizes or fragmented SD cards |
| 24-bit / 192 kHz    | NO         | Don't attempt; resample offline            |

Recommended scope: **16-bit/44.1–48 kHz as guaranteed tier; 24-bit/44.1–48 kHz as supported; 96 kHz+ as best-effort with graceful degradation (skip if underrun).**

Block size note: FLAC files with very large block sizes (>8192 samples) increase per-frame decode time. Standard FLAC tools default to 4096, which is fine.

### 2. WAV / PCM — BASELINE

Decoder: Built-in (trivial header parse + raw PCM read)

All reasonable configurations are real-time. Only constraint is SD read speed for very high bitrate files (24-bit/192 kHz stereo = ~1.1 MB/s), which exceeds reliable SD throughput on DS/DSi.

Supported: 8-bit unsigned, 16-bit signed, 24-bit signed (truncated to 16). Mono and stereo. Any sample rate up to 48 kHz.

### 3. MP3 — SOLID

Decoder: **minimp3** (CC0 public domain, single-header)

Concern: minimp3 uses `float` internally. On ARM9 without FPU, all float ops are GCC soft-float (~10-50x slower than hardware float). However:

- At 133 MHz DSi-mode ARM9, soft-float MP3 decoding of 128–320 kbps stereo MP3 is **feasible** — moonshell and other DS homebrew decoded MP3 at 67 MHz using libmad (integer-only).
- minimp3's float path is lighter than a full libmad build and the 2x clock compensates for soft-float overhead.
- If minimp3 proves too slow on real hardware, swap to **libmad** (GPL, integer-only) as a drop-in replacement at the decoder interface level.

Verdict: **Solid at all standard bitrates (32–320 kbps CBR/VBR).** If soft-float proves tight, libmad is the fallback.

### 4. Ogg Vorbis — STUBBED (FUTURE)

Decoder: **Tremor** (BSD license, integer-only Vorbis) or **stb_vorbis** (public domain, but float-heavy)

Tremor is the correct choice for FPU-less ARM but requires building as a library from the Xiph.org source. This project includes a stub decoder interface; integrating Tremor is straightforward but not included in the initial build.

At 133 MHz with Tremor (integer), Vorbis q5–q8 (~160–256 kbps) should decode in real-time. Higher quality settings are best-effort.

## Feature Feasibility

| Feature              | Status     | Notes |
|----------------------|------------|-------|
| Play/Pause/Stop      | SOLID      | Core state machine |
| Next/Previous        | SOLID      | |
| Filesystem browser   | SOLID      | libfat; 16 MB RAM allows caching large directories |
| Playlist (M3U)       | SOLID      | Load/save/reorder |
| Seek bar             | SOLID for WAV/FLAC (sample-accurate seek); BEST-EFFORT for MP3 (frame scanning required) |
| Volume control       | SOLID      | Hardware channel volume via libnds |
| Shuffle/repeat       | SOLID      | |
| Metadata display     | SOLID      | ID3v1/v2 for MP3, FLAC Vorbis comments, WAV info |
| Album art            | SOLID on DSi | 16 MB RAM easily holds a 256x192 decoded image alongside audio buffers — not feasible on 4 MB NTR mode |
| Visualizer           | BEST-EFFORT | Simple waveform/VU meter OK; FFT-based spectrum may compete with decoder for CPU |
| Battery indicator    | SOLID      | libnds `getBatteryLevel()` |
| Persistent settings  | SOLID      | Simple INI file on SD |

## Licensing Summary

| Library   | License       | Implication |
|-----------|---------------|-------------|
| libnds    | zlib          | Permissive  |
| libfat    | BSD           | Permissive  |
| maxmod    | ISC           | Permissive  |
| dr_flac   | Public domain | No restrictions |
| minimp3   | CC0           | No restrictions |
| Tremor    | BSD           | Permissive (if integrated later) |
| libmad    | GPL-2.0       | Copyleft — binary distribution must include source or offer. Only needed if minimp3 fallback is required |

## DSi-Specific Notes

- **DSi audio hardware**: The DSi SPU is the same as the original DS. Homebrew accesses it identically via libnds. There is no exposed high-quality DAC or improved audio path for homebrew to use.
- **DSi XL screen size**: 4.2" vs 3.25" per screen. Same 256x192 resolution, so pixels are physically larger. UI hit targets that are comfortable on original DS are even easier on XL. No code changes needed, but the larger screen makes a clean, spacious UI layout more pleasant.
- **SD card access**: DSi XL has no GBA slot. All storage is via the internal SD slot. libfat handles this transparently via `fatInitDefault()` in DSi mode.
- **16 MB RAM**: The single biggest DSi advantage for a media player. Allows generous audio buffers (32K+ samples), full album art decode, large directory caches, and comfortable playlist sizes without swapping.
