// Pictures onto a panel's buffer: nearest at a whole scale, or "sharp
// bilinear" (mGBA's look, as the 3DS port uses: the picture blown up to the
// largest whole scale by nearest, then smoothed to size - so only the edges
// between source pixels are blended and the pixels stay crisp).
#ifndef BANANA_SCALE_H
#define BANANA_SCALE_H

#include <stdbool.h>
#include <stdint.h>

#include "display.h"

struct scale_rect {
    int x, y, w, h;
};

// Where a sw x sh picture goes on a pw x ph panel: `whole` = the largest
// whole scale that fits (centred), else fitted keeping its shape, or
// `stretch` = the whole panel.
struct scale_rect scale_fit(int sw, int sh, int pw, int ph, bool whole, bool stretch);

// Draws src (sw x sh, `sstride` pixels a row, 0x00RRGGBB) into r on the
// buffer, and clears the rest of it when the buffer last held another
// rectangle. `swapRB`: the source is 0x00BBGGRR (mGBA's 32-bit colour).
void scale_draw(struct display_buf* b, int pw, int ph, const uint32_t* src, int sw, int sh,
                int sstride, struct scale_rect r, bool swapRB);

#endif
