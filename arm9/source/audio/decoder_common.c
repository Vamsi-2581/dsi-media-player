#include "decoder.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static bool hasExtension(const char *path, const char *ext) {
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    const char *p = path + plen - elen;
    for (size_t i = 0; i < elen; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        char e = ext[i];
        if (e >= 'A' && e <= 'Z') e += 32;
        if (c != e) return false;
    }
    return true;
}

AudioFormat detectFormat(const char *path) {
    if (hasExtension(path, ".wav") || hasExtension(path, ".wave"))
        return FORMAT_WAV;
    if (hasExtension(path, ".flac"))
        return FORMAT_FLAC;
    if (hasExtension(path, ".mp3"))
        return FORMAT_MP3;
    if (hasExtension(path, ".ogg") || hasExtension(path, ".oga"))
        return FORMAT_VORBIS;
    return FORMAT_UNKNOWN;
}

bool decoderInit(Decoder *dec, const char *path) {
    memset(dec, 0, sizeof(Decoder));
    AudioFormat fmt = detectFormat(path);

    switch (fmt) {
        case FORMAT_WAV:    decoderInitWav(dec);  break;
        case FORMAT_FLAC:   decoderInitFlac(dec); break;
        case FORMAT_MP3:    decoderInitMp3(dec);  break;
        case FORMAT_VORBIS: decoderInitVorbis(dec); break;
        default: return false;
    }

    dec->format = fmt;
    dec->info.format = fmt;
    DecodeResult res = dec->open(dec, path);
    if (res != DECODE_OK) {
        if (dec->close) dec->close(dec);
        return false;
    }
    return true;
}

void decoderCleanup(Decoder *dec) {
    if (dec->close) dec->close(dec);
    memset(dec, 0, sizeof(Decoder));
}

/* Fast ID3v2 tag parser — shared with decoder_mp3.c but duplicated
   here to avoid exposing static functions */
static void metaParseId3v2(FILE *f, AudioMeta *meta) {
    fseek(f, 0, SEEK_SET);
    u8 header[10];
    if (fread(header, 1, 10, f) != 10) return;
    if (memcmp(header, "ID3", 3) != 0) return;

    u32 tagSize = ((header[6] & 0x7F) << 21) |
                  ((header[7] & 0x7F) << 14) |
                  ((header[8] & 0x7F) << 7)  |
                   (header[9] & 0x7F);
    u32 tagEnd = 10 + tagSize;
    u8 majorVer = header[3];

    while ((u32)ftell(f) + 10 <= tagEnd) {
        u8 fh[10];
        if (fread(fh, 1, 10, f) != 10) break;
        if (fh[0] == 0) break;

        u32 frameSize;
        if (majorVer >= 4) {
            frameSize = ((fh[4] & 0x7F) << 21) | ((fh[5] & 0x7F) << 14) |
                        ((fh[6] & 0x7F) << 7)  |  (fh[7] & 0x7F);
        } else {
            frameSize = (fh[4] << 24) | (fh[5] << 16) | (fh[6] << 8) | fh[7];
        }
        if (frameSize == 0 || frameSize > tagSize) break;

        char *dest = NULL;
        u32 maxLen = META_MAX_LEN - 1;

        if (memcmp(fh, "TIT2", 4) == 0) dest = meta->title;
        else if (memcmp(fh, "TPE1", 4) == 0) dest = meta->artist;
        else if (memcmp(fh, "TALB", 4) == 0) dest = meta->album;
        else if (memcmp(fh, "TRCK", 4) == 0) { dest = meta->trackNum; maxLen = 7; }
        else if (memcmp(fh, "TDRC", 4) == 0 || memcmp(fh, "TYER", 4) == 0)
            { dest = meta->year; maxLen = 7; }
        else if (memcmp(fh, "TCON", 4) == 0) dest = meta->genre;
        else if (memcmp(fh, "APIC", 4) == 0) {
            meta->hasArt = true;
            meta->artOffset = ftell(f);
            meta->artSize = frameSize;
            fseek(f, frameSize, SEEK_CUR);
            continue;
        }

        if (dest && frameSize > 1) {
            u8 encoding = 0;
            fread(&encoding, 1, 1, f);
            u32 textLen = frameSize - 1;
            if (textLen > maxLen) textLen = maxLen;
            if (encoding == 0 || encoding == 3) {
                fread(dest, 1, textLen, f);
                dest[textLen] = '\0';
                if (textLen < frameSize - 1) fseek(f, frameSize - 1 - textLen, SEEK_CUR);
            } else {
                fseek(f, frameSize - 1, SEEK_CUR);
            }
        } else {
            fseek(f, frameSize, SEEK_CUR);
        }
    }
}

static void metaParseId3v1(FILE *f, AudioMeta *meta) {
    fseek(f, -128, SEEK_END);
    u8 tag[128];
    if (fread(tag, 1, 128, f) != 128) return;
    if (memcmp(tag, "TAG", 3) != 0) return;

    char buf[31];
    memcpy(buf, tag + 3, 30); buf[30] = '\0';
    for (int i = 29; i >= 0 && buf[i] == ' '; i--) buf[i] = '\0';
    if (buf[0] && !meta->title[0]) strncpy(meta->title, buf, META_MAX_LEN - 1);

    memcpy(buf, tag + 33, 30); buf[30] = '\0';
    for (int i = 29; i >= 0 && buf[i] == ' '; i--) buf[i] = '\0';
    if (buf[0] && !meta->artist[0]) strncpy(meta->artist, buf, META_MAX_LEN - 1);

    memcpy(buf, tag + 63, 30); buf[30] = '\0';
    for (int i = 29; i >= 0 && buf[i] == ' '; i--) buf[i] = '\0';
    if (buf[0] && !meta->album[0]) strncpy(meta->album, buf, META_MAX_LEN - 1);
}

static bool metaParseMp3(FILE *f, AudioMeta *meta, AudioInfo *info) {
    info->format = FORMAT_MP3;
    info->bitsPerSample = 16;
    metaParseId3v2(f, meta);
    metaParseId3v1(f, meta);

    /* Quick scan for first MP3 frame header to get sample rate/bitrate */
    u32 id3End = 0;
    fseek(f, 0, SEEK_SET);
    u8 hdr[10];
    if (fread(hdr, 1, 10, f) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        id3End = 10 + (((hdr[6]&0x7F)<<21)|((hdr[7]&0x7F)<<14)|((hdr[8]&0x7F)<<7)|(hdr[9]&0x7F));
    }

    fseek(f, id3End, SEEK_SET);
    u8 scanBuf[512];
    u32 scanRead = fread(scanBuf, 1, sizeof(scanBuf), f);
    u8 buf[4];
    bool found = false;
    for (u32 i = 0; i + 3 < scanRead; i++) {
        if (scanBuf[i] != 0xFF) continue;
        if ((scanBuf[i+1] & 0xE0) != 0xE0) continue;
        buf[0] = scanBuf[i]; buf[1] = scanBuf[i+1];
        buf[2] = scanBuf[i+2]; buf[3] = scanBuf[i+3];
        found = true;
        break;
    }
    if (found) {

        static const u16 sampleRates[] = {44100, 48000, 32000, 0};
        static const u16 bitrates3[][16] = {
            {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0},
            {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0},
            {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0}
        };

        int layer = 4 - ((buf[1] >> 1) & 3);
        int brIdx = (buf[2] >> 4) & 0xF;
        int srIdx = (buf[2] >> 2) & 3;
        int ch = (buf[3] >> 6) & 3;

        if (layer >= 1 && layer <= 3 && srIdx < 3 && brIdx > 0 && brIdx < 15) {
            info->sampleRate = sampleRates[srIdx];
            info->channels = (ch == 3) ? 1 : 2;
            int bitrate = bitrates3[layer-1][brIdx];

            fseek(f, 0, SEEK_END);
            u32 fileSize = ftell(f);
            u32 audioBytes = fileSize - id3End;
            if (bitrate > 0) {
                info->durationMs = (u32)((u64)audioBytes * 8 * 1000 / ((u64)bitrate * 1000));
                info->totalSamples = (u32)((u64)info->durationMs * info->sampleRate / 1000);
            }
        }
    }
    return true;
}

static bool metaParseFlac(FILE *f, AudioMeta *meta, AudioInfo *info) {
    info->format = FORMAT_FLAC;
    fseek(f, 0, SEEK_SET);
    u8 hdr[4];
    if (fread(hdr, 1, 4, f) != 4) return false;
    if (memcmp(hdr, "fLaC", 4) != 0) return false;

    bool last = false;
    while (!last) {
        u8 blockHdr[4];
        if (fread(blockHdr, 1, 4, f) != 4) break;
        last = (blockHdr[0] & 0x80) != 0;
        u8 blockType = blockHdr[0] & 0x7F;
        u32 blockLen = (blockHdr[1] << 16) | (blockHdr[2] << 8) | blockHdr[3];

        long blockStart = ftell(f);

        if (blockType == 0 && blockLen >= 34) {
            u8 si[34];
            if (fread(si, 1, 34, f) == 34) {
                info->sampleRate = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
                info->channels = ((si[12] >> 1) & 7) + 1;
                info->bitsPerSample = (((si[12] & 1) << 4) | (si[13] >> 4)) + 1;
                u64 totalSamples = ((u64)(si[13] & 0xF) << 32) |
                                   (si[14] << 24) | (si[15] << 16) | (si[16] << 8) | si[17];
                info->totalSamples = (u32)totalSamples;
                if (info->sampleRate > 0)
                    info->durationMs = (u32)(totalSamples * 1000 / info->sampleRate);
            }
        } else if (blockType == 4 && blockLen <= 65536) {
            /* Vorbis comment — bulk read entire block */
            u8 *blk = malloc(blockLen);
            if (!blk) goto skip;
            if (fread(blk, 1, blockLen, f) != blockLen) { free(blk); goto skip; }

            u32 pos = 0;
            if (pos + 4 > blockLen) { free(blk); goto skip; }
            u32 vendorLen = blk[pos] | (blk[pos+1]<<8) | (blk[pos+2]<<16) | (blk[pos+3]<<24);
            pos += 4 + vendorLen;
            if (pos + 4 > blockLen) { free(blk); goto skip; }
            u32 numComments = blk[pos] | (blk[pos+1]<<8) | (blk[pos+2]<<16) | (blk[pos+3]<<24);
            pos += 4;

            char cbuf[META_MAX_LEN + 32];
            for (u32 i = 0; i < numComments && pos + 4 <= blockLen; i++) {
                u32 clen = blk[pos] | (blk[pos+1]<<8) | (blk[pos+2]<<16) | (blk[pos+3]<<24);
                pos += 4;
                if (pos + clen > blockLen) break;
                u32 copyLen = (clen < sizeof(cbuf) - 1) ? clen : sizeof(cbuf) - 1;
                memcpy(cbuf, blk + pos, copyLen);
                cbuf[copyLen] = '\0';
                pos += clen;

                char *eq = strchr(cbuf, '=');
                if (!eq) continue;
                *eq = '\0';
                char *val = eq + 1;

                for (char *p = cbuf; *p; p++) { if (*p >= 'A' && *p <= 'Z') *p += 32; }

                char *dest = NULL;
                if (strcmp(cbuf, "title") == 0) dest = meta->title;
                else if (strcmp(cbuf, "artist") == 0) dest = meta->artist;
                else if (strcmp(cbuf, "album") == 0) dest = meta->album;
                else if (strcmp(cbuf, "tracknumber") == 0) dest = meta->trackNum;
                else if (strcmp(cbuf, "date") == 0) dest = meta->year;
                else if (strcmp(cbuf, "genre") == 0) dest = meta->genre;

                if (dest && !dest[0]) {
                    strncpy(dest, val, META_MAX_LEN - 1);
                    dest[META_MAX_LEN - 1] = '\0';
                }
            }
            free(blk);
        } else if (blockType == 6) {
            meta->hasArt = true;
            meta->artOffset = blockStart;
            meta->artSize = blockLen;
        }
skip:
        fseek(f, blockStart + blockLen, SEEK_SET);
    }
    return true;
}

static bool metaParseWav(FILE *f, AudioMeta *meta, AudioInfo *info) {
    (void)meta;
    info->format = FORMAT_WAV;
    fseek(f, 0, SEEK_SET);
    u8 hdr[44];
    if (fread(hdr, 1, 44, f) < 36) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    info->channels = hdr[22] | (hdr[23] << 8);
    info->sampleRate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    info->bitsPerSample = hdr[34] | (hdr[35] << 8);

    fseek(f, 0, SEEK_END);
    u32 fileSize = ftell(f);
    u32 dataSize = fileSize - 44;
    u32 bytesPerSample = info->channels * (info->bitsPerSample / 8);
    if (bytesPerSample > 0 && info->sampleRate > 0) {
        info->totalSamples = dataSize / bytesPerSample;
        info->durationMs = (u32)((u64)info->totalSamples * 1000 / info->sampleRate);
    }
    return true;
}

bool decoderParseMeta(const char *path, AudioMeta *meta, AudioInfo *info) {
    memset(meta, 0, sizeof(AudioMeta));
    memset(info, 0, sizeof(AudioInfo));

    AudioFormat fmt = detectFormat(path);
    if (fmt == FORMAT_UNKNOWN) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    bool ok = false;
    switch (fmt) {
        case FORMAT_MP3:  ok = metaParseMp3(f, meta, info); break;
        case FORMAT_FLAC: ok = metaParseFlac(f, meta, info); break;
        case FORMAT_WAV:  ok = metaParseWav(f, meta, info); break;
        default: break;
    }

    fclose(f);
    return ok;
}
