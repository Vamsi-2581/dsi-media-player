#ifndef JPEGTHUMB_H
#define JPEGTHUMB_H

#ifndef _NDS_H_
#include <nds.h>
#endif
#include <stdbool.h>
#include <stdint.h>

#ifndef u8
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

/*
 * Decode a JPEG image to a small RGB555 thumbnail using only DC coefficients.
 * Uses ~6KB of stack and zero heap — safe for the DS even with huge source images.
 *
 * data/len: compressed JPEG data
 * out:      output buffer, must hold outW*outH u16 values
 * outW/outH: desired thumbnail dimensions (e.g., 64x64)
 *
 * Returns true on success.
 */
bool jpegThumbDecode(const u8 *data, u32 len, u16 *out, int outW, int outH);
int jpegThumbDbgCode(void);

#endif
