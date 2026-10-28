/* Quick test for jpegthumb decoder - compile and run on Mac */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Stub NDS types for Mac compilation */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define RGB15(r,g,b) (((r)&31)|((g)&31)<<5|((b)&31)<<10)
#define BIT(n) (1u<<(n))

/* Provide nds.h stubs and include decoder */
#define _NDS_H_
#include "../arm9/source/ui/jpegthumb.c"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <jpeg_file>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    u8 *data = malloc(len);
    fread(data, 1, len, f);
    fclose(f);

    printf("File: %s (%ld bytes)\n", argv[1], len);
    printf("Header: %02X %02X\n", data[0], data[1]);

    /* Parse markers to check */
    JpegCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    u32 scanStart = 0;
    bool parsed = parseMarkers(&ctx, data, len, &scanStart);
    printf("parseMarkers: %s\n", parsed ? "OK" : "FAIL");
    if (parsed) {
        printf("  Image: %dx%d, %d components\n", ctx.width, ctx.height, ctx.ncomp);
        printf("  MaxH=%d MaxV=%d MCUs=%dx%d\n", ctx.maxH, ctx.maxV, ctx.mcuCols, ctx.mcuRows);
        printf("  Scan starts at offset %u\n", scanStart);
        for (int i = 0; i < ctx.ncomp; i++) {
            printf("  Comp %d: hSamp=%d vSamp=%d qtab=%d dcTab=%d acTab=%d\n",
                   i, ctx.comp[i].hSamp, ctx.comp[i].vSamp,
                   ctx.comp[i].qtab, ctx.comp[i].dcTab, ctx.comp[i].acTab);
        }
        printf("  Q[0] tables: Y=%d Cb=%d Cr=%d\n",
               ctx.qtab[ctx.comp[0].qtab][0],
               ctx.ncomp > 1 ? ctx.qtab[ctx.comp[1].qtab][0] : 0,
               ctx.ncomp > 2 ? ctx.qtab[ctx.comp[2].qtab][0] : 0);
        printf("  Restart interval: %d\n", ctx.restartInterval);
    }

    u16 out[64 * 64];
    memset(out, 0, sizeof(out));
    bool ok = jpegThumbDecode(data, len, out, 64, 64);
    printf("jpegThumbDecode: %s\n", ok ? "OK" : "FAIL");

    if (ok) {
        /* Write a simple PPM to verify visually */
        FILE *ppm = fopen("thumb_test.ppm", "wb");
        fprintf(ppm, "P6\n64 64\n255\n");
        for (int i = 0; i < 64 * 64; i++) {
            u16 c = out[i];
            u8 r = (c & 0x1F) << 3;
            u8 g = ((c >> 5) & 0x1F) << 3;
            u8 b = ((c >> 10) & 0x1F) << 3;
            fwrite(&r, 1, 1, ppm);
            fwrite(&g, 1, 1, ppm);
            fwrite(&b, 1, 1, ppm);
        }
        fclose(ppm);
        printf("Wrote thumb_test.ppm\n");

        /* Show some pixel values */
        printf("Pixels [0,0]=%04X [32,32]=%04X [63,63]=%04X\n",
               out[0], out[32*64+32], out[63*64+63]);

        /* Show RGB values for center pixel */
        u16 cp = out[32*64+32];
        printf("Center pixel RGB555: r=%d g=%d b=%d\n",
               cp & 0x1F, (cp>>5)&0x1F, (cp>>10)&0x1F);
    }

    free(data);
    return 0;
}
