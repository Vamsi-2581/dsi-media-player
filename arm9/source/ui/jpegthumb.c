#include "jpegthumb.h"
#include <string.h>

#define M_SOF0 0xC0
#define M_SOF2 0xC2
#define M_DHT  0xC4
#define M_DQT  0xDB
#define M_DRI  0xDD
#define M_SOS  0xDA
#define M_EOI  0xD9

static int jpegDbgCode;
int jpegThumbDbgCode(void) { return jpegDbgCode; }

#define MAX_COMP 3
#define MAX_MCU_W 160  /* supports up to 2560px wide at 4:2:0 */

typedef struct {
    int mincode[17];
    int maxcode[17];
    int valptr[17];
    u8 sym[256];
    /* Fast 8-bit lookup: high byte = bits consumed, low byte = symbol.
     * If bits == 0, code is longer than 8 bits — use slow path. */
    u16 fast[256];
} HuffTab;

typedef struct {
    const u8 *data;
    u32 len;
    u32 pos;
    u32 buf;
    int cnt;
} Bits;

typedef struct {
    u8 hSamp, vSamp;
    u8 qtab;
    u8 dcTab, acTab;
} CompInfo;

typedef struct {
    int width, height;
    int ncomp;
    CompInfo comp[MAX_COMP];
    int maxH, maxV;
    int mcuCols, mcuRows;
    HuffTab dcHuff[2];
    HuffTab acHuff[2];
    u16 qtab[4][64];
    int restartInterval;
    int dcPred[MAX_COMP];
    int scanOrder[MAX_COMP];
    int nScanComp;
    int spectralEnd;  /* Se from SOS: 0 = DC-only scan (progressive), 63 = full (baseline) */
    int approxLow;    /* Al from SOS: successive approximation bit position */
    Bits bits;
} JpegCtx;

static void bitsInit(Bits *b, const u8 *data, u32 len, u32 pos) {
    b->data = data;
    b->len = len;
    b->pos = pos;
    b->buf = 0;
    b->cnt = 0;
}

static int bitsReadByte(Bits *b) {
    if (b->pos >= b->len) return -1;
    u8 c = b->data[b->pos++];
    if (c == 0xFF) {
        if (b->pos >= b->len) return -1;
        u8 next = b->data[b->pos];
        if (next == 0x00) {
            b->pos++;
            return 0xFF;
        }
        return -1;
    }
    return c;
}

static bool bitsFill(Bits *b, int n) {
    while (b->cnt < n) {
        int byte = bitsReadByte(b);
        if (byte < 0) return false;
        b->buf = (b->buf << 8) | (u32)byte;
        b->cnt += 8;
    }
    return true;
}

static int bitsRead(Bits *b, int n) {
    if (n == 0) return 0;
    if (!bitsFill(b, n)) return -1;
    b->cnt -= n;
    return (int)((b->buf >> b->cnt) & ((1u << n) - 1));
}

static void buildHuff(HuffTab *h, const u8 *counts, const u8 *symbols) {
    int code = 0;
    int si = 0;
    memset(h->fast, 0, sizeof(h->fast));

    for (int len = 1; len <= 16; len++) {
        h->mincode[len] = code;
        h->valptr[len] = si;
        int n = counts[len - 1];
        for (int i = 0; i < n; i++) {
            u8 sym = *symbols++;
            h->sym[si] = sym;
            /* Populate fast lookup for codes <= 8 bits */
            if (len <= 8) {
                int prefix = code << (8 - len);
                int fill = 1 << (8 - len);
                for (int f = 0; f < fill; f++)
                    h->fast[prefix + f] = (u16)((len << 8) | sym);
            }
            si++;
            code++;
        }
        h->maxcode[len] = (n > 0) ? code - 1 : -1;
        code <<= 1;
    }
}

static int huffDecode(Bits *b, HuffTab *h) {
    /* Fast path: peek 8 bits and use lookup table.
     * Save state so we can revert if bitsFill reads past a restart marker. */
    u32 savedPos = b->pos;
    u32 savedBuf = b->buf;
    int savedCnt = b->cnt;

    if (bitsFill(b, 8)) {
        int peek = (int)((b->buf >> (b->cnt - 8)) & 0xFF);
        u16 entry = h->fast[peek];
        if (entry) {
            b->cnt -= (entry >> 8);
            return entry & 0xFF;
        }
        /* Code > 8 bits: consume the 8 peeked bits, continue from bit 9 */
        int code = peek;
        b->cnt -= 8;
        for (int len = 9; len <= 16; len++) {
            int bit = bitsRead(b, 1);
            if (bit < 0) return -1;
            code = (code << 1) | bit;
            if (h->maxcode[len] >= 0 && code <= h->maxcode[len])
                return h->sym[h->valptr[len] + code - h->mincode[len]];
        }
        return -1;
    }

    /* bitsFill failed — restore state and try bit-by-bit */
    b->pos = savedPos;
    b->buf = savedBuf;
    b->cnt = savedCnt;

    int code = 0;
    for (int len = 1; len <= 16; len++) {
        int bit = bitsRead(b, 1);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len])
            return h->sym[h->valptr[len] + code - h->mincode[len]];
    }
    return -1;
}

static int extend(int v, int nbits) {
    if (nbits == 0) return 0;
    if (v < (1 << (nbits - 1)))
        v -= (1 << nbits) - 1;
    return v;
}

static bool skipRestart(Bits *b) {
    b->cnt = 0;
    b->buf = 0;
    while (b->pos < b->len - 1) {
        if (b->data[b->pos] == 0xFF) {
            u8 m = b->data[b->pos + 1];
            if (m >= 0xD0 && m <= 0xD7) {
                b->pos += 2;
                return true;
            }
            if (m != 0x00)
                return false;
        }
        b->pos++;
    }
    return false;
}

static int decodeDC(JpegCtx *ctx, int ci) {
    HuffTab *dcH = &ctx->dcHuff[ctx->comp[ci].dcTab];

    int cat = huffDecode(&ctx->bits, dcH);
    if (cat < 0) return 0x80000000;

    int diff = 0;
    if (cat > 0) {
        diff = bitsRead(&ctx->bits, cat);
        if (diff < 0) return 0x80000000;
        diff = extend(diff, cat);
    }
    ctx->dcPred[ci] += diff;
    int dc = ctx->dcPred[ci];

    /* Apply successive approximation shift for progressive JPEG */
    if (ctx->approxLow > 0)
        dc <<= ctx->approxLow;

    dc *= (int)ctx->qtab[ctx->comp[ci].qtab][0];

    /* Skip AC coefficients only in baseline/full scan (Se=63) */
    if (ctx->spectralEnd > 0) {
        HuffTab *acH = &ctx->acHuff[ctx->comp[ci].acTab];
        int count = 1;
        while (count < 64) {
            int rs = huffDecode(&ctx->bits, acH);
            if (rs < 0) return 0x80000000;
            if (rs == 0x00) break;
            int run = (rs >> 4) & 0x0F;
            int size = rs & 0x0F;
            count += run + 1;
            if (size > 0 && bitsRead(&ctx->bits, size) < 0)
                return 0x80000000;
        }
    }

    return dc;
}

static int clamp255(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static bool parseMarkers(JpegCtx *ctx, const u8 *data, u32 len, u32 *scanStart) {
    if (len < 2 || data[0] != 0xFF || data[1] != 0xD8) return false;

    u32 pos = 2;
    bool gotSOF = false;

    while (pos < len - 1) {
        if (data[pos] != 0xFF) { pos++; continue; }
        while (pos < len && data[pos] == 0xFF) pos++;
        if (pos >= len) break;
        u8 marker = data[pos++];

        if (marker == 0 || marker == 0xD8 || marker == M_EOI) continue;
        if (marker >= 0xD0 && marker <= 0xD7) continue;

        if (pos + 2 > len) break;
        u32 mlen = ((u32)data[pos] << 8) | data[pos + 1];
        if (mlen < 2 || pos + mlen > len) break;
        const u8 *seg = data + pos + 2;
        u32 segLen = mlen - 2;

        if (marker == M_SOF0 || marker == M_SOF2) {
            if (segLen < 6) return false;
            ctx->height = (seg[1] << 8) | seg[2];
            ctx->width = (seg[3] << 8) | seg[4];
            ctx->ncomp = seg[5];
            if (ctx->ncomp < 1 || ctx->ncomp > MAX_COMP) return false;
            if (segLen < (u32)(6 + ctx->ncomp * 3)) return false;
            ctx->maxH = 1;
            ctx->maxV = 1;
            for (int i = 0; i < ctx->ncomp; i++) {
                u8 samp = seg[6 + i * 3 + 1];
                ctx->comp[i].hSamp = (samp >> 4) & 0x0F;
                ctx->comp[i].vSamp = samp & 0x0F;
                ctx->comp[i].qtab = seg[6 + i * 3 + 2];
                if (ctx->comp[i].hSamp > (u8)ctx->maxH) ctx->maxH = ctx->comp[i].hSamp;
                if (ctx->comp[i].vSamp > (u8)ctx->maxV) ctx->maxV = ctx->comp[i].vSamp;
            }
            ctx->mcuCols = (ctx->width + ctx->maxH * 8 - 1) / (ctx->maxH * 8);
            ctx->mcuRows = (ctx->height + ctx->maxV * 8 - 1) / (ctx->maxV * 8);
            gotSOF = true;

        } else if (marker == M_DHT) {
            u32 p = 0;
            while (p < segLen) {
                u8 info = seg[p++];
                int cls = (info >> 4) & 1;
                int id = info & 0x0F;
                if (id > 1) return false;
                if (p + 16 > segLen) return false;
                u8 counts[16];
                int total = 0;
                for (int i = 0; i < 16; i++) {
                    counts[i] = seg[p++];
                    total += counts[i];
                }
                if (p + (u32)total > segLen) return false;
                HuffTab *h = cls ? &ctx->acHuff[id] : &ctx->dcHuff[id];
                buildHuff(h, counts, seg + p);
                p += total;
            }

        } else if (marker == M_DQT) {
            u32 p = 0;
            while (p < segLen) {
                u8 info = seg[p++];
                int prec = (info >> 4) & 0x0F;
                int id = info & 0x0F;
                if (id > 3) return false;
                for (int i = 0; i < 64; i++) {
                    if (prec) {
                        if (p + 2 > segLen) return false;
                        ctx->qtab[id][i] = (u16)((seg[p] << 8) | seg[p + 1]);
                        p += 2;
                    } else {
                        if (p >= segLen) return false;
                        ctx->qtab[id][i] = seg[p++];
                    }
                }
            }

        } else if (marker == M_DRI) {
            if (segLen >= 2)
                ctx->restartInterval = (seg[0] << 8) | seg[1];

        } else if (marker == M_SOS) {
            if (!gotSOF || segLen < 1) return false;
            int ns = seg[0];
            if (ns < 1 || ns > MAX_COMP) return false;
            if (segLen < (u32)(1 + ns * 2 + 3)) return false;
            ctx->nScanComp = ns;
            for (int i = 0; i < ns; i++) {
                u8 tabs = seg[1 + i * 2 + 1];
                ctx->scanOrder[i] = (i < ctx->ncomp) ? i : 0;
                int ci = ctx->scanOrder[i];
                ctx->comp[ci].dcTab = (tabs >> 4) & 0x0F;
                ctx->comp[ci].acTab = tabs & 0x0F;
            }
            int ssOff = 1 + ns * 2;
            ctx->spectralEnd = seg[ssOff + 1];  /* Se */
            ctx->approxLow = seg[ssOff + 2] & 0x0F;  /* Al */
            *scanStart = pos + mlen;
            return true;
        }

        pos += mlen;
    }
    return false;
}

bool jpegThumbDecode(const u8 *data, u32 len, u16 *out, int outW, int outH) {
    jpegDbgCode = 0;
    JpegCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    u32 scanStart = 0;
    if (!parseMarkers(&ctx, data, len, &scanStart)) {
        jpegDbgCode = 1;
        return false;
    }

    if (ctx.mcuCols > MAX_MCU_W || ctx.mcuCols < 1) {
        jpegDbgCode = 2;
        return false;
    }

    int yW = ctx.mcuCols * ctx.maxH;
    int yH = ctx.mcuRows * ctx.maxV;
    if (yW > MAX_MCU_W * 2 || yH < 1) {
        jpegDbgCode = 3;
        return false;
    }

    bitsInit(&ctx.bits, data, len, scanStart);

    int dcY[MAX_MCU_W * 2 * 2];  /* [maxV][maxH * mcuCols], max 2*320=640 entries */
    int dcCb[MAX_MCU_W];
    int dcCr[MAX_MCU_W];

    memset(out, 0, (u32)(outW * outH) * sizeof(u16));

    /* For non-interleaved scans (Ns=1), MCU structure differs:
     * each MCU is one 8x8 block, and we iterate over all blocks of that component. */
    bool interleaved = (ctx.nScanComp > 1);

    /* Non-interleaved single-component scan: decode just that component's blocks */
    if (!interleaved) {
        int ci = ctx.scanOrder[0];
        int blocksX, blocksY;

        if (ci == 0) {
            blocksX = ctx.mcuCols * ctx.comp[0].hSamp;
            blocksY = ctx.mcuRows * ctx.comp[0].vSamp;
        } else {
            blocksX = ctx.mcuCols * ctx.comp[ci].hSamp;
            blocksY = ctx.mcuRows * ctx.comp[ci].vSamp;
        }

        int rstCount = 0;
        for (int by = 0; by < blocksY; by++) {
            if (ci == 0) memset(dcY, 0, sizeof(dcY));

            for (int bx = 0; bx < blocksX; bx++) {
                if (ctx.restartInterval > 0 && rstCount > 0 &&
                    (rstCount % ctx.restartInterval) == 0) {
                    if (!skipRestart(&ctx.bits)) {
                        jpegDbgCode = 100 + by;
                        return false;
                    }
                    ctx.dcPred[ci] = 0;
                }

                int dc = decodeDC(&ctx, ci);
                if (dc == (int)0x80000000) {
                    jpegDbgCode = 1000 + by * 10 + ci;
                    return false;
                }

                if (ci == 0 && bx < MAX_MCU_W * 2)
                    dcY[bx] = dc;
                rstCount++;
            }

            /* Map this block row to output (Y only for non-interleaved) */
            if (ci == 0) {
                for (int oy = 0; oy < outH; oy++) {
                    int srcRow = oy * blocksY / outH;
                    if (srcRow != by) continue;
                    for (int ox = 0; ox < outW; ox++) {
                        int srcCol = ox * blocksX / outW;
                        if (srcCol >= blocksX) srcCol = blocksX - 1;
                        int lum = clamp255(dcY[srcCol] / 8 + 128);
                        int v = lum >> 3;
                        out[oy * outW + ox] = RGB15(v, v, v) | BIT(15);
                    }
                }
            }
        }
        /* Non-interleaved: we only got one component (grayscale-ish for Y) */
        return true;
    }

    /* Interleaved scan: standard MCU processing */
    int rstCount = 0;

    for (int mcuRow = 0; mcuRow < ctx.mcuRows; mcuRow++) {
        memset(dcY, 0, sizeof(dcY));
        memset(dcCb, 0, sizeof(dcCb));
        memset(dcCr, 0, sizeof(dcCr));

        for (int mcuCol = 0; mcuCol < ctx.mcuCols; mcuCol++) {
            if (ctx.restartInterval > 0 && rstCount > 0 &&
                (rstCount % ctx.restartInterval) == 0) {
                if (!skipRestart(&ctx.bits)) {
                    jpegDbgCode = 100 + mcuRow;
                    return false;
                }
                ctx.dcPred[0] = ctx.dcPred[1] = ctx.dcPred[2] = 0;
            }

            for (int si = 0; si < ctx.nScanComp; si++) {
                int ci = ctx.scanOrder[si];
                int nblocks = ctx.comp[ci].hSamp * ctx.comp[ci].vSamp;

                for (int bi = 0; bi < nblocks; bi++) {
                    int dc = decodeDC(&ctx, ci);
                    if (dc == (int)0x80000000) {
                        jpegDbgCode = 1000 + mcuRow * 10 + si;
                        return false;
                    }

                    if (ci == 0) {
                        int bx = bi % ctx.comp[0].hSamp;
                        int bby = bi / ctx.comp[0].hSamp;
                        int idx = bby * yW + mcuCol * ctx.maxH + bx;
                        if (idx < MAX_MCU_W * 2 * 2)
                            dcY[idx] = dc;
                    } else if (ci == 1) {
                        dcCb[mcuCol] = dc;
                    } else {
                        dcCr[mcuCol] = dc;
                    }
                }
            }
            rstCount++;
        }

        /* Map this MCU row's DC values to output pixels */
        int blockRowStart = mcuRow * ctx.maxV;
        int blockRowEnd = blockRowStart + ctx.maxV;

        for (int oy = 0; oy < outH; oy++) {
            int srcRow = oy * yH / outH;
            if (srcRow < blockRowStart || srcRow >= blockRowEnd) continue;
            int localRow = srcRow - blockRowStart;

            for (int ox = 0; ox < outW; ox++) {
                int srcCol = ox * yW / outW;
                if (srcCol >= yW) srcCol = yW - 1;

                int yIdx = localRow * yW + srcCol;
                int yVal = (yIdx < MAX_MCU_W * 2 * 2) ? dcY[yIdx] : 0;

                /* DC / 8 + 128 = average pixel value */
                int lum = yVal / 8 + 128;

                if (ctx.ncomp >= 3) {
                    int cbCol = srcCol * ctx.mcuCols / yW;
                    int crCol = cbCol;
                    if (cbCol >= ctx.mcuCols) cbCol = ctx.mcuCols - 1;
                    if (crCol >= ctx.mcuCols) crCol = ctx.mcuCols - 1;

                    int cbVal = dcCb[cbCol];
                    int crVal = dcCr[crCol];

                    /* cb/cr are dequantized DC values; /8 gives chroma offset from 128 */
                    int cbOff = cbVal / 8;  /* Cb - 128 */
                    int crOff = crVal / 8;  /* Cr - 128 */

                    /* YCbCr -> RGB (fixed-point, ×1024) */
                    int r = clamp255(lum + (1436 * crOff >> 10));
                    int g = clamp255(lum - ((352 * cbOff + 731 * crOff) >> 10));
                    int b = clamp255(lum + (1815 * cbOff >> 10));

                    out[oy * outW + ox] = RGB15(r >> 3, g >> 3, b >> 3) | BIT(15);
                } else {
                    int v = clamp255(lum) >> 3;
                    out[oy * outW + ox] = RGB15(v, v, v) | BIT(15);
                }
            }
        }
    }

    return true;
}
