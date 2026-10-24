// See input.h. Device detection follows banana-ui's (ui/src/input.cpp in
// BananaLinux), so what the menu treats as a touchscreen or a pad, this does too.
#include "input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BITS_LONG (8 * sizeof(long))
#define NLONGS(n) ((n) / BITS_LONG + 1)

static bool test_bit(const unsigned long* bits, int n) {
    return (bits[n / BITS_LONG] >> (n % BITS_LONG)) & 1;
}

static uint32_t map_key(int code) {
    switch (code) {
    case BTN_DPAD_LEFT: case KEY_LEFT: return IN_LEFT;
    case BTN_DPAD_RIGHT: case KEY_RIGHT: return IN_RIGHT;
    case BTN_DPAD_UP: case KEY_UP: return IN_UP;
    case BTN_DPAD_DOWN: case KEY_DOWN: return IN_DOWN;
    case BTN_EAST: case KEY_ENTER: return IN_A; // the RG DS's A is east (Nintendo layout)
    case BTN_SOUTH: case KEY_BACKSPACE: case KEY_ESC: return IN_B;
    case BTN_NORTH: case KEY_X: return IN_X;
    case BTN_WEST: case KEY_Y: return IN_Y;
    case BTN_TL: case KEY_PAGEUP: return IN_L;
    case BTN_TR: case KEY_PAGEDOWN: return IN_R;
    case BTN_TL2: case KEY_HOME: return IN_L2;
    case BTN_TR2: case KEY_SPACE: return IN_R2;
    case BTN_START: case KEY_S: return IN_START;
    case BTN_SELECT: case KEY_TAB: return IN_SELECT;
    case BTN_MODE: case KEY_F1: return IN_MENU;
    case BTN_THUMBL: return IN_L3;
    case BTN_THUMBR: return IN_R3;
    case KEY_VOLUMEUP: case KEY_VOLUMEDOWN: return IN_OTHER;
    default: return 0;
    }
}

// The device's real path in sysfs, to tell the two touchscreens apart.
static void sys_path(const char* node, char* out, size_t len) {
    char link[512], real[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/class/input/%s/device", node);
    if (realpath(link, real)) snprintf(out, len, "%s", real);
    else out[0] = 0;
}

static void open_devices(struct input* in) {
    DIR* d = opendir("/dev/input");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && in->count < INPUT_MAX_DEVS) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[NLONGS(KEY_MAX)] = { 0 }, abs[NLONGS(ABS_MAX)] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
        struct input_dev dv;
        memset(&dv, 0, sizeof(dv));
        dv.fd = fd;
        bool mt = test_bit(abs, ABS_MT_POSITION_X) && test_bit(abs, ABS_MT_POSITION_Y);
        bool st = !mt && test_bit(abs, ABS_X) && test_bit(abs, ABS_Y) &&
                  (test_bit(keys, BTN_TOUCH) || (test_bit(keys, BTN_LEFT) && !test_bit(keys, BTN_SOUTH)));
        if (mt || st) {
            char sp[PATH_MAX];
            sys_path(e->d_name, sp, sizeof(sp));
            // only the bottom panel's: the top one's touchscreen does nothing here
            if (!in->bottomTouch[0] || !strstr(sp, in->bottomTouch)) {
                close(fd);
                continue;
            }
            dv.touch = true;
            dv.pointer = st && !test_bit(keys, BTN_TOUCH);
            // one finger, from one source: a second finger lifting must not end the first one's touch
            dv.emulated = test_bit(keys, BTN_TOUCH) && test_bit(abs, ABS_X) && test_bit(abs, ABS_Y);
            struct input_absinfo ax = { 0 }, ay = { 0 };
            ioctl(fd, EVIOCGABS(mt && !dv.emulated ? ABS_MT_POSITION_X : ABS_X), &ax);
            ioctl(fd, EVIOCGABS(mt && !dv.emulated ? ABS_MT_POSITION_Y : ABS_Y), &ay);
            dv.minX = ax.minimum;
            dv.maxX = ax.maximum;
            dv.minY = ay.minimum;
            dv.maxY = ay.maximum;
            // where the finger last was (a touch at the same spot sends no new position)
            struct input_absinfo px = { 0 }, py = { 0 };
            if (test_bit(abs, ABS_X) && test_bit(abs, ABS_Y) && ioctl(fd, EVIOCGABS(ABS_X), &px) == 0 &&
                ioctl(fd, EVIOCGABS(ABS_Y), &py) == 0) {
                dv.x = px.value;
                dv.y = py.value;
            } else {
                dv.x = ax.value;
                dv.y = ay.value;
            }
        } else {
            bool any = test_bit(abs, ABS_HAT0X);
            for (int k = KEY_ESC; k < KEY_MAX && !any; k++) {
                if (test_bit(keys, k) && map_key(k)) any = true;
            }
            if (!any) {
                close(fd);
                continue;
            }
            // a pad's left stick works as the d-pad
            if (test_bit(abs, ABS_X) && test_bit(abs, ABS_Y)) {
                struct input_absinfo sx = { 0 }, sy = { 0 };
                if (ioctl(fd, EVIOCGABS(ABS_X), &sx) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &sy) == 0 &&
                    sx.maximum > sx.minimum && sy.maximum > sy.minimum) {
                    dv.stick = true;
                    dv.sMin[0] = sx.minimum;
                    dv.sMax[0] = sx.maximum;
                    dv.sVal[0] = sx.value;
                    dv.sMin[1] = sy.minimum;
                    dv.sMax[1] = sy.maximum;
                    dv.sVal[1] = sy.value;
                }
            }
        }
        in->dev[in->count++] = dv;
    }
    closedir(d);
}

void input_open(struct input* in, const char* bottomTouch, int panelW, int panelH) {
    memset(in, 0, sizeof(*in));
    snprintf(in->bottomTouch, sizeof(in->bottomTouch), "%s", bottomTouch ? bottomTouch : "");
    in->panelW = panelW;
    in->panelH = panelH;
    in->watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (in->watch >= 0) inotify_add_watch(in->watch, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB);
    open_devices(in);
}

void input_close(struct input* in) {
    for (int i = 0; i < in->count; i++) if (in->dev[i].fd >= 0) close(in->dev[i].fd);
    in->count = 0;
    if (in->watch >= 0) close(in->watch);
    in->watch = -1;
}

// A stick axis past half way, as d-pad bits.
static uint32_t stick_bits(const struct input_dev* d) {
    uint32_t b = 0;
    for (int a = 0; a < 2; a++) {
        int mid = (d->sMin[a] + d->sMax[a]) / 2, half = (d->sMax[a] - d->sMin[a]) / 4;
        if (d->sVal[a] < mid - half) b |= a ? IN_UP : IN_LEFT;
        if (d->sVal[a] > mid + half) b |= a ? IN_DOWN : IN_RIGHT;
    }
    return b;
}

// After SYN_DROPPED (the kernel's buffer overflowed: a release may be lost), the device's state as it is now.
static void resync(struct input_dev* d) {
    unsigned long keys[NLONGS(KEY_MAX)] = { 0 };
    ioctl(d->fd, EVIOCGKEY(sizeof(keys)), keys);
    if (d->touch) {
        if (d->emulated) d->down = test_bit(keys, BTN_TOUCH);
        else if (d->pointer) d->down = test_bit(keys, BTN_LEFT);
        else d->down = false; // (the next contact reports itself again)
        struct input_absinfo a = { 0 };
        if (ioctl(d->fd, EVIOCGABS(d->emulated || d->pointer ? ABS_X : ABS_MT_POSITION_X), &a) == 0) d->x = a.value;
        if (ioctl(d->fd, EVIOCGABS(d->emulated || d->pointer ? ABS_Y : ABS_MT_POSITION_Y), &a) == 0) d->y = a.value;
        return;
    }
    d->keys = 0;
    for (int k = 0; k < KEY_MAX; k++) {
        if (test_bit(keys, k)) d->keys |= map_key(k);
    }
    struct input_absinfo hx = { 0 }, hy = { 0 };
    d->hat = 0;
    if (ioctl(d->fd, EVIOCGABS(ABS_HAT0X), &hx) == 0) d->hat |= hx.value < 0 ? IN_LEFT : (hx.value > 0 ? IN_RIGHT : 0);
    if (ioctl(d->fd, EVIOCGABS(ABS_HAT0Y), &hy) == 0) d->hat |= hy.value < 0 ? IN_UP : (hy.value > 0 ? IN_DOWN : 0);
    if (d->stick) {
        struct input_absinfo s = { 0 };
        if (ioctl(d->fd, EVIOCGABS(ABS_X), &s) == 0) d->sVal[0] = s.value;
        if (ioctl(d->fd, EVIOCGABS(ABS_Y), &s) == 0) d->sVal[1] = s.value;
    }
}

void input_poll(struct input* in) {
    if (in->watch >= 0) {
        char buf[4096];
        bool changed = false;
        while (read(in->watch, buf, sizeof(buf)) > 0) changed = true;
        if (changed) {
            // a device came: everything opened again (held keys start released)
            for (int i = 0; i < in->count; i++) if (in->dev[i].fd >= 0) close(in->dev[i].fd);
            in->count = 0;
            open_devices(in);
        }
    }
    uint32_t held = 0;
    for (int i = 0; i < in->count; i++) {
        struct input_dev* d = &in->dev[i];
        struct input_event ev[64];
        ssize_t n;
        if (d->fd < 0) continue;
        while ((n = read(d->fd, ev, sizeof(ev))) > 0) {
            for (int k = 0; k < n / (ssize_t) sizeof(*ev); k++) {
                const struct input_event* e = &ev[k];
                if (e->type == EV_SYN && e->code == SYN_DROPPED) {
                    resync(d);
                    continue;
                }
                if (d->touch) {
                    if (e->type == EV_ABS && e->code == ABS_MT_SLOT) {
                        d->slot = e->value;
                    } else if (d->emulated || d->pointer) {
                        if (e->type == EV_ABS && e->code == ABS_X) d->x = e->value;
                        else if (e->type == EV_ABS && e->code == ABS_Y) d->y = e->value;
                        else if (e->type == EV_KEY && e->code == (d->pointer ? BTN_LEFT : BTN_TOUCH)) d->down = e->value != 0;
                    } else if (d->slot == 0 && e->type == EV_ABS) {
                        if (e->code == ABS_MT_POSITION_X) d->x = e->value;
                        else if (e->code == ABS_MT_POSITION_Y) d->y = e->value;
                        else if (e->code == ABS_MT_TRACKING_ID) d->down = e->value >= 0;
                    }
                    continue;
                }
                if (e->type == EV_KEY && e->value != 2) {
                    uint32_t b = map_key(e->code);
                    if (e->value) d->keys |= b;
                    else d->keys &= ~b;
                } else if (e->type == EV_ABS && (e->code == ABS_HAT0X || e->code == ABS_HAT0Y)) {
                    uint32_t neg = e->code == ABS_HAT0X ? IN_LEFT : IN_UP, pos = e->code == ABS_HAT0X ? IN_RIGHT : IN_DOWN;
                    d->hat &= ~(neg | pos);
                    if (e->value < 0) d->hat |= neg;
                    if (e->value > 0) d->hat |= pos;
                } else if (e->type == EV_ABS && d->stick && (e->code == ABS_X || e->code == ABS_Y)) {
                    d->sVal[e->code == ABS_Y] = e->value;
                }
            }
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            // gone (a pad unplugged: ENODEV): closed, and nothing it held stays held
            close(d->fd);
            d->fd = -1;
            d->keys = d->hat = 0;
            d->down = d->stick = false;
            continue;
        }
        if (d->touch) {
            if (d->down) {
                float sx = d->maxX > d->minX ? (float) in->panelW / (d->maxX - d->minX + 1) : 1;
                float sy = d->maxY > d->minY ? (float) in->panelH / (d->maxY - d->minY + 1) : 1;
                in->touchX = (int) ((d->x - d->minX) * sx);
                in->touchY = (int) ((d->y - d->minY) * sy);
            }
        } else {
            held |= d->keys | d->hat | (d->stick ? stick_bits(d) : 0);
        }
    }
    bool touching = false;
    int kept = 0;
    for (int i = 0; i < in->count; i++) {
        if (in->dev[i].fd < 0) continue;
        if (in->dev[i].touch && in->dev[i].down) touching = true;
        in->dev[kept++] = in->dev[i];
    }
    in->count = kept;
    in->held = held;
    in->touching = touching;
}
