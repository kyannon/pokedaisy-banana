// The buttons and the bottom touchscreen, from evdev (docs/EMULATORS.md: the
// pad is read, never grabbed - bananad reads the same device for the lid, the
// power button and the volume keys). Keyboards count too (QEMU, a desk), with
// banana-ui's keys.
#ifndef BANANA_INPUT_H
#define BANANA_INPUT_H

#include <stdbool.h>
#include <stdint.h>

// Buttons, a bit each.
enum {
    IN_A = 1 << 0, IN_B = 1 << 1, IN_X = 1 << 2, IN_Y = 1 << 3,
    IN_L = 1 << 4, IN_R = 1 << 5, IN_L2 = 1 << 6, IN_R2 = 1 << 7,
    IN_START = 1 << 8, IN_SELECT = 1 << 9,
    IN_UP = 1 << 10, IN_DOWN = 1 << 11, IN_LEFT = 1 << 12, IN_RIGHT = 1 << 13,
    IN_MENU = 1 << 14, // F (BTN_MODE)
    IN_L3 = 1 << 15, IN_R3 = 1 << 16,
    IN_OTHER = 1 << 17, // a key that's someone else's (the volume keys: bananad's), held
};

#define INPUT_MAX_DEVS 16

struct input_dev {
    int fd;
    bool touch;
    bool pointer; // an absolute pointer (QEMU's tablet): BTN_LEFT is the touch
    bool emulated; // BTN_TOUCH with ABS_X / ABS_Y (the kernel's single-touch view): read alone, no ABS_MT_*
    int slot;      // multitouch without that view: the slot being reported (only slot 0 counts)
    int minX, maxX, minY, maxY;
    int x, y;
    bool down;
    // a stick: its range, for the d-pad
    bool stick;
    int sMin[2], sMax[2], sVal[2];
    uint32_t keys, hat;
};

struct input {
    struct input_dev dev[INPUT_MAX_DEVS];
    int count;
    int watch; // inotify on /dev/input: a pad or keyboard plugged in
    char bottomTouch[128];
    int panelW, panelH;
    // what the frame reads
    uint32_t held;
    bool touching;
    int touchX, touchY; // panel pixels
};

// Opens every event device; the touchscreen whose sysfs path has `bottomTouch`
// in it is the bottom panel's (the other is the top panel's, unused).
void input_open(struct input* in, const char* bottomTouch, int panelW, int panelH);
void input_close(struct input* in);
// Reads everything waiting (non-blocking), updating held / touching.
void input_poll(struct input* in);

#endif
