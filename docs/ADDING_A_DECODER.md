# Adding an audio format

Walkthrough for wiring a new decoder in. `decoder_wav.c` is the simplest
existing one and the best reference; `decoder_flac.c` shows the
third-party-library pattern.

## The constraint that decides everything

**The ARM9 has no FPU.** Pick an integer / fixed-point decoder. A
float-based library will compile and run, but every sample goes through
software float emulation and the ring buffer will starve, producing audible
clicks and dropouts.

This is not theoretical — it is why MP3 uses Helix rather than minimp3. See
[ARCHITECTURE.md](ARCHITECTURE.md#why-helix-for-mp3).

Also check the license before you invest time. Anything GPL would force the
whole ROM to GPL; the project is MIT with the Helix subtree carved out.

## 1. Add the format enum

`arm9/source/audio/decoder.h`:

```c
typedef enum {
    FORMAT_UNKNOWN = 0,
    FORMAT_WAV,
    FORMAT_FLAC,
    FORMAT_MP3,
    FORMAT_VORBIS,
    FORMAT_OPUS,        /* new */
} AudioFormat;
```

Declare the initialiser alongside the others:

```c
void decoderInitOpus(Decoder *dec);
```

## 2. Register the extension and dispatch

`arm9/source/audio/decoder_common.c`. Detection is **extension-based**:

```c
AudioFormat detectFormat(const char *path) {
    ...
    if (hasExtension(path, ".opus"))
        return FORMAT_OPUS;
    return FORMAT_UNKNOWN;
}
```

and in `decoderInit()`:

```c
case FORMAT_OPUS: decoderInitOpus(dec); break;
```

## 3. Let the browser and library see the files

Both the browser and the library scanner gate on a single predicate,
`isAudioFile()` in `arm9/source/fs/file_browser.c`:

```c
bool isAudioFile(const char *name) {
    return hasExt(name, ".wav") || hasExt(name, ".wave") ||
           hasExt(name, ".flac") ||
           hasExt(name, ".mp3") ||
           hasExt(name, ".ogg") || hasExt(name, ".oga") ||
           hasExt(name, ".opus");          /* new */
}
```

Add your extension there and both surfaces pick it up — `library.c` calls
the same function when deciding what to index. Miss this and the decoder
works but the files are invisible in the UI.

Note this is separate from `detectFormat()`: `isAudioFile()` decides what is
*listed*, `detectFormat()` decides what *decodes* it. Both need the
extension.

## 4. Write the decoder

New file `arm9/source/audio/decoder_opus.c`. It is picked up automatically —
`arm9/Makefile` globs `source/audio`, so no Makefile edit is needed for a
`.c` in an existing source directory.

```c
#include "decoder.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    /* your decoder state */
    s16  pcm[4096 * 2];   /* interleaved stereo scratch */
    u32  pcmFill;         /* frames currently buffered */
    u32  pcmPos;          /* frames already handed out */
    u32  curFrame;        /* absolute position, for tell() */
} OpusCtx;

static DecodeResult opusOpen(Decoder *dec, const char *path) {
    dec->file = fopen(path, "rb");
    if (!dec->file) return DECODE_ERROR;

    OpusCtx *ctx = calloc(1, sizeof(OpusCtx));
    if (!ctx) { fclose(dec->file); dec->file = NULL; return DECODE_ERROR; }
    dec->ctx = ctx;

    /* parse headers, then fill in: */
    dec->info.sampleRate   = 48000;
    dec->info.channels     = 2;
    dec->info.bitsPerSample = 16;
    dec->info.totalSamples = /* frames, or 0 if unknown */ 0;
    dec->info.durationMs   = dec->info.totalSamples
                           ? (u32)((u64)dec->info.totalSamples * 1000
                                   / dec->info.sampleRate)
                           : 0;
    return DECODE_OK;
}

static DecodeResult opusRead(Decoder *dec, s16 *outBuf,
                             u32 numSamples, u32 *samplesRead) {
    OpusCtx *ctx = dec->ctx;
    u32 done = 0;

    while (done < numSamples) {
        if (ctx->pcmPos >= ctx->pcmFill) {
            /* decode one packet into ctx->pcm; set pcmFill, reset pcmPos */
            u32 got = decode_one_packet(ctx);
            if (got == 0) break;          /* end of stream */
            ctx->pcmFill = got;
            ctx->pcmPos  = 0;
        }
        u32 avail = ctx->pcmFill - ctx->pcmPos;
        u32 want  = numSamples - done;
        u32 n     = (avail < want) ? avail : want;

        memcpy(outBuf + done * 2,
               ctx->pcm + ctx->pcmPos * 2,
               n * 2 * sizeof(s16));

        ctx->pcmPos  += n;
        ctx->curFrame += n;
        done         += n;
    }

    *samplesRead = done;
    return (done == 0) ? DECODE_EOF : DECODE_OK;
}

static DecodeResult opusSeek(Decoder *dec, u32 samplePos) {
    OpusCtx *ctx = dec->ctx;
    /* reposition the file, reset the scratch buffer */
    ctx->pcmFill = ctx->pcmPos = 0;
    ctx->curFrame = samplePos;
    return DECODE_OK;
}

static u32  opusTell(Decoder *dec) { return ((OpusCtx *)dec->ctx)->curFrame; }

static void opusClose(Decoder *dec) {
    free(dec->ctx);
    dec->ctx = NULL;
    if (dec->file) { fclose(dec->file); dec->file = NULL; }
}

void decoderInitOpus(Decoder *dec) {
    dec->open        = opusOpen;
    dec->readSamples = opusRead;
    dec->seek        = opusSeek;
    dec->tell        = opusTell;
    dec->close       = opusClose;
}
```

### Contract details that matter

- **Output is interleaved stereo `s16`, always.** Upmix mono yourself by
  duplicating into both channels. Everything downstream assumes stereo.
- **`numSamples` counts frames, not shorts.** A request for `n` frames
  wants `2n` `s16` values written. Getting this wrong gives you half-speed
  or double-speed audio.
- **Short reads are fine.** Set `*samplesRead` to what you produced and
  return `DECODE_OK`. Return `DECODE_EOF` only when the stream is actually
  over. Returning `DECODE_EOF` with a partial buffer loses that audio.
- **`seek`/`tell` are in frames** from the start of the stream.
- **`close` must be idempotent-safe.** `decoderInit()` calls it if `open()`
  fails, so it can run on a partially-initialised struct. Null out what you
  free.
- **`totalSamples` may legitimately be 0** if the container does not say.
  The seek bar degrades to showing elapsed time only.

### Buffering

`audio_stream.c` asks for `DECODE_CHUNK` (1024) frames at a time, but
packet-based codecs decode in their own fixed block sizes. Keep an internal
scratch buffer and hand out slices, as above — do not assume the request
size lines up with your packet size.

Keep large scratch buffers either `static` or heap-allocated, not on the
stack. Stack space is limited and a few KB of `s16` on it will overflow.

## 5. Metadata

`decoderParseMeta()` in `decoder_common.c` reads tags and locates embedded
cover art **without** opening a decoder, because the library scanner runs it
across every file on the card. Add a `metaParseOpus()` branch following
`metaParseFlac()`.

Fill in `AudioMeta`: `title`, `artist`, `album`, `trackNum`, `year`,
`genre`, and for art `hasArt` / `artOffset` / `artSize` — the offset and
size of the **raw image bytes** within the file. `cover_art.c` seeks there
and hands the bytes to `jpegthumb.c`, so only baseline JPEG art will
actually display.

## 6. Third-party library placement

If you are vendoring a decoder library:

- Permissive license (MIT/BSD/zlib/public domain) → `libs/<name>/`, and add
  it to `INCLUDES` in `arm9/Makefile`.
- Single header fetched at setup time → add it to `setup.sh` and
  `.gitignore`, like `dr_flac.h`.
- Anything with copyleft-ish terms → keep it in its own `libs/` subtree with
  its license intact, document it in `libs/README.md` and `LICENSE`, and do
  not mix it with project code. `libs/helix/` is the precedent.

Wrap the library in one translation unit (`helix_mp3.c` does this — it
`#include`s the vendored `.c` files) so you can keep its warnings isolated
from the project build.

## 7. Test it

Compile clean with no new warnings, then:

1. Emulator ([melonDS](https://melonds.kuribo64.net/) in DSi mode) — catches
   crashes and obvious logic errors.
2. **Real hardware** — the only way to know you are not underrunning.
   Emulators do not reproduce SD card latency or true CPU timing.

Exercise: different sample rates (44.1 and 48 kHz at least), mono and
stereo, seeking to the middle and near the end, playing to the natural end
of a track, a file with tags, a file without, and a deliberately truncated
or corrupt file (it should fail to open, not hang or crash).

Check that the seek bar tracks correctly and that lyrics stay in sync — both
depend on `tell()` being accurate.
