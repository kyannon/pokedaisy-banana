// The two panels: KMS dumb buffers on BananaOS (docs/EMULATORS.md in
// BananaLinux: DRM master, both panels' modes as they are, page flips on
// vblank, all given back on exit), or a headless pair of buffers in memory for
// the tests, which write them out as PNGs.
//
// Each panel has two buffers. display_begin waits until the panel's last flip
// is done and hands back the hidden one (0x00RRGGBB, `pitch` bytes a row);
// display_end flips it on at the next vblank. Waiting for the top panel's flip
// is what paces the game to the panels' 60 Hz.
#ifndef BANANA_DISPLAY_H
#define BANANA_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PANEL_TOP, PANEL_BOTTOM, PANELS };

struct display_buf {
    uint32_t handle, fb, pitch; // pitch in bytes (KMS)
    size_t size;
    uint32_t* px;
    int rect[4]; // the last picture's rectangle in it (x, y, w, h); the rest is black
};

struct display {
    bool headless;
    int fd;
    int w, h;          // both panels' size
    double refresh;    // Hz, from the top panel's mode
    struct {
        uint32_t conn, crtc;
        uint8_t mode[68]; // struct drm_mode_modeinfo
        struct display_buf buf[2];
        int front;
        bool pending; // a flip queued and not done yet
        bool flips;   // page flips work (else each frame is set directly)
        int flipFails; // flips refused in a row
    } p[PANELS];
};

// The panels on /dev/dri/card*: two connected connectors of the type named in
// `top` ("DSI-2"; "Virtual-2" in QEMU), that one the top panel.
bool display_open_kms(struct display* d, const char* top, char* err, size_t errLen);
// Two panels of w x h in memory.
bool display_open_headless(struct display* d, int w, int h);
void display_close(struct display* d);

// The panel's hidden buffer, once its last flip is done (waits at most a few
// frames: a flip lost across a suspend doesn't hang the game).
struct display_buf* display_begin(struct display* d, int panel);
void display_end(struct display* d, int panel);

// Writes a panel's shown picture as a PNG (headless; also works on KMS).
bool display_png(struct display* d, int panel, const char* path);

#endif
