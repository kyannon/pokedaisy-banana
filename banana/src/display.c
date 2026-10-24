// The two panels over the kernel's DRM interface directly (no libdrm, like
// the rest of this host: nothing to link but libc). See display.h.
#include "display.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "../../3ds/desktop/pd_png.h"

_Static_assert(sizeof(struct drm_mode_modeinfo) == 68, "mode size");

#define XRGB8888 0x34325258 // 'XR24'

// The kernel's connector type names (drm_connector.c), for "DSI-2" and the like.
static const char* const CONNECTOR_NAMES[] = {
    "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS", "Component", "DIN", "DP",
    "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB",
};

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static uint64_t ptr(const void* p) {
    return (uint64_t) (uintptr_t) p;
}

static bool make_buf(struct display* d, struct display_buf* b) {
    struct drm_mode_create_dumb c = { .width = (uint32_t) d->w, .height = (uint32_t) d->h, .bpp = 32 };
    if (xioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return false;
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = (size_t) c.size;
    struct drm_mode_fb_cmd2 fb = { .width = (uint32_t) d->w, .height = (uint32_t) d->h, .pixel_format = XRGB8888 };
    fb.handles[0] = b->handle;
    fb.pitches[0] = b->pitch;
    if (xioctl(d->fd, DRM_IOCTL_MODE_ADDFB2, &fb)) return false;
    b->fb = fb.fb_id;
    struct drm_mode_map_dumb m = { .handle = b->handle };
    if (xioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return false;
    void* p = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t) m.offset);
    if (p == MAP_FAILED) return false;
    b->px = p;
    memset(b->px, 0, b->size);
    return true;
}

static void free_buf(struct display* d, struct display_buf* b) {
    if (d->headless) {
        free(b->px);
    } else {
        if (b->px) munmap(b->px, b->size);
        if (b->fb) xioctl(d->fd, DRM_IOCTL_MODE_RMFB, &b->fb);
        if (b->handle) {
            struct drm_mode_destroy_dumb x = { .handle = b->handle };
            xioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &x);
        }
    }
    memset(b, 0, sizeof(*b));
}

struct found {
    uint32_t conn, crtc;
    struct drm_mode_modeinfo mode;
    bool top;
};

// The connector's modes and encoders (the two-call pattern: counts, then
// the arrays; again if they changed in between).
static bool get_connector(int fd, uint32_t id, struct drm_mode_get_connector* c, struct drm_mode_modeinfo** modes,
                          uint32_t** encoders) {
    *modes = NULL;
    *encoders = NULL;
    for (int tries = 0; tries < 4; tries++) {
        memset(c, 0, sizeof(*c));
        c->connector_id = id;
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, c)) return false;
        uint32_t nm = c->count_modes, ne = c->count_encoders;
        free(*modes);
        free(*encoders);
        *modes = calloc(nm ? nm : 1, sizeof(**modes));
        *encoders = calloc(ne ? ne : 1, sizeof(**encoders));
        struct drm_mode_get_connector again = { .connector_id = id };
        again.count_modes = nm;
        again.modes_ptr = ptr(*modes);
        again.count_encoders = ne;
        again.encoders_ptr = ptr(*encoders);
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &again)) return false;
        if (again.count_modes == nm && again.count_encoders == ne) {
            *c = again;
            return true;
        }
    }
    return false;
}

// Two panels of the top one's type on this card: false when it hasn't got them.
static bool find_panels(int fd, const char* top, const char* type, struct found out[2], char* err, size_t errLen) {
    struct drm_mode_card_res res = { 0 };
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return false;
    uint32_t ncrtc = res.count_crtcs, nconn = res.count_connectors;
    uint32_t* crtcs = calloc(ncrtc + 1, sizeof(uint32_t));
    uint32_t* conns = calloc(nconn + 1, sizeof(uint32_t));
    struct drm_mode_card_res r2 = { 0 };
    r2.count_crtcs = ncrtc;
    r2.crtc_id_ptr = ptr(crtcs);
    r2.count_connectors = nconn;
    r2.connector_id_ptr = ptr(conns);
    int n = 0;
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2) == 0) {
        if (r2.count_crtcs < ncrtc) ncrtc = r2.count_crtcs;
        if (r2.count_connectors < nconn) nconn = r2.count_connectors;
        uint32_t usedCrtcs = 0;
        for (uint32_t i = 0; i < nconn && n < 2; i++) {
            struct drm_mode_get_connector c;
            struct drm_mode_modeinfo* modes;
            uint32_t* encs;
            if (!get_connector(fd, conns[i], &c, &modes, &encs)) {
                free(modes);
                free(encs);
                continue;
            }
            const char* tn = c.connector_type < sizeof(CONNECTOR_NAMES) / sizeof(*CONNECTOR_NAMES)
                                 ? CONNECTOR_NAMES[c.connector_type] : "?";
            if (!strcmp(tn, type) && c.connection == 1 /* connected */ && c.count_modes > 0) {
                // the mode as the panel has it: its preferred one (the device tree's timing; never another)
                struct drm_mode_modeinfo mode = modes[0];
                for (uint32_t m = 0; m < c.count_modes; m++) {
                    if (modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                        mode = modes[m];
                        break;
                    }
                }
                for (uint32_t m = 0; m < c.count_modes; m++) {
                    if (modes[m].type & DRM_MODE_TYPE_USERDEF) {
                        mode = modes[m];
                        break;
                    }
                }
                // a CRTC its encoders can drive that the other panel hasn't got
                uint32_t crtc = 0;
                for (uint32_t e = 0; e < c.count_encoders && !crtc; e++) {
                    struct drm_mode_get_encoder enc = { .encoder_id = encs[e] };
                    if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc)) continue;
                    for (uint32_t k = 0; k < ncrtc && k < 32 && !crtc; k++) {
                        if ((enc.possible_crtcs & (1u << k)) && !(usedCrtcs & (1u << k))) {
                            crtc = crtcs[k];
                            usedCrtcs |= 1u << k;
                        }
                    }
                }
                if (crtc) {
                    char name[32];
                    snprintf(name, sizeof(name), "%s-%u", tn, c.connector_type_id);
                    out[n].conn = c.connector_id;
                    out[n].crtc = crtc;
                    out[n].mode = mode;
                    out[n].top = !strcmp(name, top);
                    n++;
                }
            }
            free(modes);
            free(encs);
        }
    }
    free(crtcs);
    free(conns);
    if (n < 2) {
        snprintf(err, errLen, "no two connected %s panels", type);
        return false;
    }
    // panel 0 is the top: the named connector, else the second one (as banana-ui)
    if (!out[0].top && !out[1].top) {
        struct found t = out[0];
        out[0] = out[1];
        out[1] = t;
    }
    if (out[1].top) {
        struct found t = out[0];
        out[0] = out[1];
        out[1] = t;
    }
    if (out[0].mode.hdisplay != out[1].mode.hdisplay || out[0].mode.vdisplay != out[1].mode.vdisplay) {
        snprintf(err, errLen, "the two panels have different modes");
        return false;
    }
    return true;
}

static bool set_crtc(struct display* d, int s, uint32_t fb) {
    struct drm_mode_crtc c = { 0 };
    c.crtc_id = d->p[s].crtc;
    c.fb_id = fb;
    c.set_connectors_ptr = ptr(&d->p[s].conn);
    c.count_connectors = 1;
    c.mode_valid = 1;
    memcpy(&c.mode, d->p[s].mode, sizeof(c.mode));
    if (xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &c) == 0) return true;
    fprintf(stderr, "pokedaisy: setting panel %d's picture failed (%s)\n", s, strerror(errno));
    return false;
}

bool display_open_kms(struct display* d, const char* top, char* err, size_t errLen) {
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    char type[32];
    snprintf(type, sizeof(type), "%s", top);
    char* dash = strchr(type, '-');
    if (dash) *dash = 0;
    snprintf(err, errLen, "no /dev/dri/card*");

    char paths[8][300];
    int np = 0;
    DIR* dir = opendir("/dev/dri");
    if (dir) {
        struct dirent* e;
        while ((e = readdir(dir)) && np < 8) {
            if (!strncmp(e->d_name, "card", 4)) snprintf(paths[np++], sizeof(paths[0]), "/dev/dri/%.200s", e->d_name);
        }
        closedir(dir);
    }
    for (int i = 0; i < np; i++) {
        for (int j = i + 1; j < np; j++) {
            if (strcmp(paths[j], paths[i]) < 0) {
                char t[300];
                memcpy(t, paths[i], sizeof(t));
                memcpy(paths[i], paths[j], sizeof(t));
                memcpy(paths[j], t, sizeof(t));
            }
        }
    }
    for (int i = 0; i < np; i++) {
        int fd = open(paths[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct found f[2];
        memset(f, 0, sizeof(f));
        if (!find_panels(fd, top, type, f, err, errLen)) {
            close(fd);
            continue;
        }
        if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) && errno != EINVAL) {
            // (0 when this fd is master already; EINVAL from a lessee, which a lease's panels allow; anything else
            // means someone else holds the panels)
            snprintf(err, errLen, "%s: can't be DRM master (%s): is the menu still holding the panels?", paths[i],
                     strerror(errno));
            close(fd);
            return false;
        }
        d->fd = fd;
        d->w = f[0].mode.hdisplay;
        d->h = f[0].mode.vdisplay;
        double total = (double) f[0].mode.htotal * f[0].mode.vtotal;
        d->refresh = total > 0 ? f[0].mode.clock * 1000.0 / total : 60.0;
        if (d->refresh < 30 || d->refresh > 240) d->refresh = 60.0;
        for (int s = 0; s < PANELS; s++) {
            d->p[s].conn = f[s].conn;
            d->p[s].crtc = f[s].crtc;
            memcpy(d->p[s].mode, &f[s].mode, sizeof(d->p[s].mode));
            d->p[s].flips = true;
            if (!make_buf(d, &d->p[s].buf[0]) || !make_buf(d, &d->p[s].buf[1])) {
                snprintf(err, errLen, "can't make the panels' scanout buffers");
                display_close(d);
                return false;
            }
            if (!set_crtc(d, s, d->p[s].buf[0].fb)) {
                snprintf(err, errLen, "can't show a picture on panel %d (%s)", s, strerror(errno));
                display_close(d);
                return false;
            }
        }
        return true;
    }
    return false;
}

bool display_open_headless(struct display* d, int w, int h) {
    memset(d, 0, sizeof(*d));
    d->headless = true;
    d->fd = -1;
    d->w = w;
    d->h = h;
    d->refresh = 60.0;
    for (int s = 0; s < PANELS; s++) {
        for (int b = 0; b < 2; b++) {
            struct display_buf* bf = &d->p[s].buf[b];
            bf->pitch = (uint32_t) w * 4;
            bf->size = (size_t) w * h * 4;
            bf->px = calloc(1, bf->size);
            if (!bf->px) return false;
        }
    }
    return true;
}

void display_close(struct display* d) {
    for (int s = 0; s < PANELS; s++) {
        for (int b = 0; b < 2; b++) free_buf(d, &d->p[s].buf[b]);
    }
    if (d->fd >= 0) {
        // the panels back to the menu: it takes master and sets its own buffers when it opens them again
        ioctl(d->fd, DRM_IOCTL_DROP_MASTER, 0);
        close(d->fd);
    }
    d->fd = -1;
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

// Reads the flip events waiting, clearing the panels' pending flags.
static void read_events(struct display* d) {
    char buf[1024];
    ssize_t n = read(d->fd, buf, sizeof(buf));
    for (ssize_t at = 0; n > 0 && at + (ssize_t) sizeof(struct drm_event) <= n;) {
        struct drm_event* e = (struct drm_event*) (buf + at);
        if (e->length < sizeof(struct drm_event)) break;
        if (e->type == DRM_EVENT_FLIP_COMPLETE && at + (ssize_t) sizeof(struct drm_event_vblank) <= n) {
            struct drm_event_vblank* v = (struct drm_event_vblank*) e;
            int s = (int) v->user_data;
            if (s >= 0 && s < PANELS) d->p[s].pending = false;
        }
        at += e->length;
    }
}

struct display_buf* display_begin(struct display* d, int panel) {
    if (!d->headless && d->p[panel].pending) {
        // a vblank or two at the most; give up after ~6 (a flip lost across a suspend)
        long until = now_ms() + 100;
        while (d->p[panel].pending) {
            long left = until - now_ms();
            if (left <= 0) {
                d->p[panel].pending = false;
                break;
            }
            struct pollfd pf = { .fd = d->fd, .events = POLLIN };
            int r = poll(&pf, 1, (int) left);
            if (r > 0) read_events(d);
        }
    }
    return &d->p[panel].buf[d->p[panel].front ^ 1];
}

void display_end(struct display* d, int panel) {
    int back = d->p[panel].front ^ 1;
    if (d->headless) {
        d->p[panel].front = back;
        return;
    }
    struct display_buf* b = &d->p[panel].buf[back];
    if (d->p[panel].flips) {
        struct drm_mode_crtc_page_flip f = { 0 };
        f.crtc_id = d->p[panel].crtc;
        f.fb_id = b->fb;
        f.flags = DRM_MODE_PAGE_FLIP_EVENT;
        f.user_data = (uint64_t) panel;
        if (xioctl(d->fd, DRM_IOCTL_MODE_PAGE_FLIP, &f) == 0) {
            d->p[panel].pending = true;
            d->p[panel].front = back;
            d->p[panel].flipFails = 0;
            return;
        }
        // a flip refused now and then (around a resume) is set directly once; only one that keeps failing
        // turns flips off (the frames then paced by the clock)
        if (errno != EBUSY && ++d->p[panel].flipFails >= 30) {
            fprintf(stderr, "pokedaisy: page flips on panel %d keep failing (%s): setting frames directly\n", panel,
                    strerror(errno));
            d->p[panel].flips = false;
        }
    }
    // no flip (EBUSY, or flips not working): put the buffer up directly
    set_crtc(d, panel, b->fb);
    d->p[panel].front = back;
}

bool display_png(struct display* d, int panel, const char* path) {
    const struct display_buf* b = &d->p[panel].buf[d->p[panel].front];
    uint32_t* px = malloc((size_t) d->w * d->h * 4);
    if (!px) return false;
    for (int y = 0; y < d->h; y++) {
        const uint32_t* row = (const uint32_t*) ((const uint8_t*) b->px + (size_t) y * b->pitch);
        for (int x = 0; x < d->w; x++) px[y * d->w + x] = row[x] & 0xFFFFFF;
    }
    bool ok = pd_png_write(path, d->w, d->h, px);
    free(px);
    return ok;
}
