// See scale.h. Built from two lookup tables (a source pixel pair and a weight
// per output column / row): at a whole scale every weight is 0 and drawing is
// copies; in sharp bilinear only the output pixels on a source pixel's edge
// blend. Each source row is scaled across once, then the output rows are
// copies of those (or a blend of two at a row edge), written to the panel
// in order - its scanout buffer is uncached, so every byte goes once.
#include "scale.h"

#include <math.h>
#include <string.h>

#define MAX_OUT 4096
#define MAX_SRC_ROWS 256

struct axis {
    int i0[MAX_OUT], i1[MAX_OUT];
    uint16_t w[MAX_OUT]; // 0..256: the share of i1
};

static struct axis ax, ay;
static uint32_t rows[MAX_SRC_ROWS][MAX_OUT];

struct scale_rect scale_fit(int sw, int sh, int pw, int ph, bool whole, bool stretch) {
    struct scale_rect r;
    if (stretch) {
        r.x = r.y = 0;
        r.w = pw;
        r.h = ph;
        return r;
    }
    if (whole) {
        int k = pw / sw < ph / sh ? pw / sw : ph / sh;
        if (k < 1) k = 1;
        r.w = sw * k;
        r.h = sh * k;
    } else if ((long) pw * sh <= (long) ph * sw) {
        r.w = pw;
        r.h = (int) ((long) pw * sh / sw);
    } else {
        r.h = ph;
        r.w = (int) ((long) ph * sw / sh);
    }
    if (r.w > pw) r.w = pw;
    if (r.h > ph) r.h = ph;
    r.x = (pw - r.w) / 2;
    r.y = (ph - r.h) / 2;
    return r;
}

// libretro's sharp-bilinear-simple, per axis: the output pixel's centre in
// source pixels, pulled to the source pixel's centre except within half an
// output pixel of its edge; then a linear sample there.
static void build_axis(struct axis* a, int in, int out) {
    double scale = (double) out / in;
    double range = scale > 1 ? 0.5 - 0.5 / scale : 0;
    for (int o = 0; o < out; o++) {
        double texel = (o + 0.5) * in / out;
        double fl = floor(texel);
        double cd = texel - fl - 0.5;
        double cl = cd < -range ? -range : (cd > range ? range : cd);
        double f = (cd - cl) * scale + 0.5;
        if (scale <= 1) f = texel - fl; // (smaller: plain bilinear)
        double t = fl + f - 0.5;
        double i = floor(t);
        int w = (int) lround((t - i) * 256);
        int i0 = (int) i, i1 = i0 + 1;
        if (w >= 256) {
            i0 = i1;
            w = 0;
        }
        if (i0 < 0) i0 = 0;
        if (i1 < 0) i1 = 0;
        if (i0 > in - 1) i0 = in - 1;
        if (i1 > in - 1) i1 = in - 1;
        if (i1 == i0) w = 0;
        a->i0[o] = i0;
        a->i1[o] = i1;
        a->w[o] = (uint16_t) w;
    }
}

static inline uint32_t blend(uint32_t a, uint32_t b, uint32_t w) {
    uint32_t iw = 256 - w;
    uint32_t rb = (((a & 0xFF00FF) * iw + (b & 0xFF00FF) * w) >> 8) & 0xFF00FF;
    uint32_t g = (((a & 0x00FF00) * iw + (b & 0x00FF00) * w) >> 8) & 0x00FF00;
    return rb | g;
}

static inline uint32_t swap_rb(uint32_t c) {
    return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

void scale_draw(struct display_buf* b, int pw, int ph, const uint32_t* src, int sw, int sh, int sstride,
                struct scale_rect r, bool swapRB) {
    if (r.w <= 0 || r.h <= 0 || r.w > MAX_OUT || r.h > MAX_OUT || sh > MAX_SRC_ROWS) return;
    size_t pitch = b->pitch / 4;
    if (b->rect[0] != r.x || b->rect[1] != r.y || b->rect[2] != r.w || b->rect[3] != r.h) {
        // the picture moved or changed size: this buffer's old one goes
        for (int y = 0; y < ph; y++) memset(b->px + (size_t) y * pitch, 0, (size_t) pw * 4);
        b->rect[0] = r.x;
        b->rect[1] = r.y;
        b->rect[2] = r.w;
        b->rect[3] = r.h;
    }
    // (the tables are rebuilt each draw: two passes over a few hundred entries, nothing next to the pixels)
    build_axis(&ax, sw, r.w);
    build_axis(&ay, sh, r.h);

    // each source row a picture wide; only the rows the output uses
    static uint8_t needed[MAX_SRC_ROWS];
    memset(needed, 0, (size_t) sh);
    for (int y = 0; y < r.h; y++) {
        needed[ay.i0[y]] = 1;
        if (ay.w[y]) needed[ay.i1[y]] = 1;
    }
    for (int sy = 0; sy < sh; sy++) {
        if (!needed[sy]) continue;
        const uint32_t* s = src + (size_t) sy * sstride;
        uint32_t* d = rows[sy];
        for (int x = 0; x < r.w; x++) {
            uint32_t c = s[ax.i0[x]];
            if (ax.w[x]) c = blend(c, s[ax.i1[x]], ax.w[x]);
            d[x] = swapRB ? swap_rb(c) : (c & 0xFFFFFF);
        }
    }
    static uint32_t mix[MAX_OUT];
    for (int y = 0; y < r.h; y++) {
        uint32_t* out = b->px + (size_t) (r.y + y) * pitch + r.x;
        const uint32_t* a = rows[ay.i0[y]];
        if (!ay.w[y]) {
            memcpy(out, a, (size_t) r.w * 4);
        } else {
            const uint32_t* c = rows[ay.i1[y]];
            uint32_t w = ay.w[y];
            for (int x = 0; x < r.w; x++) mix[x] = blend(a[x], c[x], w);
            memcpy(out, mix, (size_t) r.w * 4);
        }
    }
}
