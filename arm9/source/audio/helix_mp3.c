/* Single compilation unit for Helix fixed-point MP3 decoder.
   All integer math — no float — designed for ARM without FPU. */

#define ARM 1

#include "coder.h"

#include "bitstream.c"
#include "buffers.c"
#include "dct32.c"
#include "dequant.c"
#include "dqchan.c"
#include "huffman.c"
#include "hufftabs.c"
#include "imdct.c"
#include "mp3dec.c"
#include "mp3tabs.c"
#include "polyphase.c"
#include "scalfact.c"
#include "stproc.c"
#include "subband.c"
#include "trigtabs.c"
