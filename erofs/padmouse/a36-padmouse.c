/* a36-padmouse: the console's gamepad as mouse and keyboard for the desktop.
 *
 *   a36-padmouse seed-udev            tag existing keyboards/mice for libinput
 *   a36-padmouse run STATE_DIR        map the pad until SIGTERM
 *
 * The pad is never grabbed: the host supervisor must keep seeing Fn+Start and
 * Select+Start. There is no udevd in a full-screen session, so this program
 * writes the udev database entries libinput needs for its own virtual device
 * (and, with seed-udev, for keyboards and mice present at start).
 *
 * Layout: left stick pointer, right stick scroll, A left click,
 * B right click, X Enter, Y on-screen keyboard, Select Escape, Start menu
 * (Super), L1 window switch (Alt+Tab), R1 Tab, L2/R2 Page Up/Down, L3 middle
 * click, R3 slow pointer, D-pad arrows. Fn+Y pauses or resumes the mapping.
 * Fn+Up/Down changes brightness. Volume keys change volume. A short press of
 * the power key asks for sleep.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP 0x220
#define BTN_DPAD_DOWN 0x221
#define BTN_DPAD_LEFT 0x222
#define BTN_DPAD_RIGHT 0x223
#endif
#define BITS(n) (((n)+8*sizeof(unsigned long)-1)/(8*sizeof(unsigned long)))
#define TEST(bits,b) (((bits)[(b)/(8*sizeof(unsigned long))]>>((b)%(8*sizeof(unsigned long))))&1UL)
#define UDEV_DB "/run/udev/data"
#define SELF_NAME "andr36oid pad pointer"
#define LIBEXEC "/usr/libexec/andr36oid-desktop"
#define MAXDEV 32

enum role { R_A, R_B, R_X, R_Y, R_L1, R_R1, R_L2, R_R2, R_L3, R_R3, R_SELECT, R_START,
            R_FN, R_UP, R_DOWN, R_LEFT, R_RIGHT, R_COUNT };
/* Native andr36oid evdev codes first, then the legacy Android-era codes. */
static const unsigned native_code[R_COUNT] = {305,304,307,308,310,311,312,313,706,707,704,705,708,544,545,546,547};
static const unsigned legacy_code[R_COUNT] = {304,305,307,308,310,311,312,313,158,125,314,315,172,103,108,105,106};

static volatile sig_atomic_t stop_flag;
static void on_stop(int s) { (void)s; stop_flag = 1; }

static long long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- udev database seeding ------------------------------------------------ */
static int read_dev_number(const char *event, unsigned *major, unsigned *minor) {
    char path[PATH_MAX]; snprintf(path, sizeof path, "/sys/class/input/%s/dev", event);
    FILE *f = fopen(path, "re"); if (!f) return -1;
    int ok = fscanf(f, "%u:%u", major, minor) == 2; fclose(f);
    return ok ? 0 : -1;
}
static void write_db(unsigned major, unsigned minor, const char *props, bool replace) {
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    mkdir("/run/udev", 0755); mkdir(UDEV_DB, 0755);
    snprintf(path, sizeof path, UDEV_DB "/c%u:%u", major, minor);
    if (!replace && access(path, F_OK) == 0) return;
    snprintf(tmp, sizeof tmp, "%s.a36", path);
    FILE *f = fopen(tmp, "we"); if (!f) return;
    fprintf(f, "I:1\nE:ID_INPUT=1\n%sG:seat\nQ:seat\nV:1\n", props);
    if (fclose(f) == 0) rename(tmp, path); else unlink(tmp);
}
enum kind { K_NONE, K_PAD, K_KEYS, K_KEYBOARD, K_MOUSE, K_SELF };
static enum kind classify(int fd) {
    unsigned long ev[BITS(EV_MAX+1)] = {0}, key[BITS(KEY_MAX+1)] = {0}, rel[BITS(REL_MAX+1)] = {0}, abs[BITS(ABS_MAX+1)] = {0};
    char name[128] = "";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    if (!strcmp(name, SELF_NAME)) return K_SELF;
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0) return K_NONE;
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    if (!strcmp(name, "GO-Super Gamepad")) return K_PAD;
    if (TEST(ev, EV_ABS) && TEST(abs, ABS_X) && (TEST(key, BTN_SOUTH) || TEST(key, BTN_EAST))) return K_PAD;
    if (TEST(ev, EV_REL) && TEST(rel, REL_X) && TEST(rel, REL_Y) && TEST(key, BTN_LEFT)) return K_MOUSE;
    if (TEST(key, KEY_A) && TEST(key, KEY_Z) && TEST(key, KEY_ENTER)) return K_KEYBOARD;
    if (TEST(key, KEY_POWER) || TEST(key, KEY_VOLUMEUP) || TEST(key, KEY_VOLUMEDOWN)) return K_KEYS;
    return K_NONE;
}
static void seed_existing(void) {
    DIR *d = opendir("/sys/class/input"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        unsigned major, minor; if (read_dev_number(e->d_name, &major, &minor)) continue;
        char node[PATH_MAX]; snprintf(node, sizeof node, "/dev/input/%s", e->d_name);
        int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        enum kind k = classify(fd); close(fd);
        /* Pads stay joysticks (libinput ignores them); power and volume keys
         * stay with this mapper so a compositor never sees them twice. */
        if (k == K_KEYBOARD) write_db(major, minor, "E:ID_INPUT_KEY=1\nE:ID_INPUT_KEYBOARD=1\n", false);
        else if (k == K_MOUSE) write_db(major, minor, "E:ID_INPUT_MOUSE=1\n", false);
    }
    closedir(d);
}

/* ---- virtual device ---------------------------------------------------------- */
static const unsigned out_keys[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, KEY_ENTER, KEY_ESC, KEY_TAB,
    KEY_LEFTALT, KEY_LEFTMETA, KEY_PAGEUP, KEY_PAGEDOWN, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT};
static int uinput_create(char *event_name, size_t size) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { perror("a36-padmouse: /dev/uinput"); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_EVBIT, EV_REL); ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (unsigned i = 0; i < sizeof out_keys / sizeof *out_keys; i++) ioctl(fd, UI_SET_KEYBIT, out_keys[i]);
    /* A full letter row makes libinput and xkb treat it as a real keyboard. */
    for (unsigned k = KEY_Q; k <= KEY_P; k++) ioctl(fd, UI_SET_KEYBIT, k);
    ioctl(fd, UI_SET_RELBIT, REL_X); ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL); ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
    struct uinput_setup setup = {.id = {.bustype = BUS_VIRTUAL, .vendor = 0x4133, .product = 0x3601, .version = 1}};
    snprintf(setup.name, sizeof setup.name, SELF_NAME);
    if (ioctl(fd, UI_DEV_SETUP, &setup) || ioctl(fd, UI_DEV_CREATE)) { perror("a36-padmouse: uinput setup"); close(fd); return -1; }
    char sysname[64] = "", path[PATH_MAX];
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sysname), sysname) < 0) { perror("a36-padmouse: sysname"); return fd; }
    for (int tries = 0; tries < 100 && !event_name[0]; tries++) {
        snprintf(path, sizeof path, "/sys/devices/virtual/input/%s", sysname);
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) if (!strncmp(e->d_name, "event", 5)) { snprintf(event_name, size, "%s", e->d_name); break; }
            closedir(d);
        }
        if (!event_name[0]) usleep(10000);
    }
    return fd;
}
static int out_fd = -1;
static void emit(unsigned type, unsigned code, int value) {
    struct input_event ev = {.type = type, .code = code, .value = value};
    if (out_fd >= 0 && write(out_fd, &ev, sizeof ev) < 0 && errno != EAGAIN) perror("a36-padmouse: write");
}
static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }
static void tap(unsigned code) { emit(EV_KEY, code, 1); syn(); emit(EV_KEY, code, 0); syn(); }

/* ---- helpers ----------------------------------------------------------------- */
static void run_helper(const char *name, const char *arg) {
    char path[PATH_MAX]; snprintf(path, sizeof path, LIBEXEC "/%s", name);
    pid_t p = fork();
    if (p == 0) { setsid(); execl(path, name, arg, (char *)NULL); _exit(127); }
}
static const char *state_dir = "/run/andr36oid-desktop";
static void toggle_osk(void) {
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/osk.pid", state_dir);
    FILE *f = fopen(path, "re"); if (!f) return;
    int pid = 0; if (fscanf(f, "%d", &pid) == 1 && pid > 1) kill(pid, SIGRTMIN);
    fclose(f);
}
static void brightness(int direction) {
    DIR *d = opendir("/sys/class/backlight"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[PATH_MAX]; long max = 0, cur = 0;
        snprintf(path, sizeof path, "/sys/class/backlight/%s/max_brightness", e->d_name);
        FILE *f = fopen(path, "re"); if (!f) continue;
        if (fscanf(f, "%ld", &max) != 1) max = 0;
        fclose(f);
        snprintf(path, sizeof path, "/sys/class/backlight/%s/brightness", e->d_name);
        f = fopen(path, "re"); if (!f) continue;
        if (fscanf(f, "%ld", &cur) != 1) cur = 0;
        fclose(f);
        if (max <= 0) continue;
        long step = max / 10 > 0 ? max / 10 : 1, next = cur + direction * step;
        if (next < 1) next = 1;          /* never fully dark: that looks like a crash */
        if (next > max) next = max;
        f = fopen(path, "we"); if (!f) continue;
        fprintf(f, "%ld\n", next); fclose(f);
    }
    closedir(d);
}

/* ---- input state ------------------------------------------------------------- */
struct axis { int min, max, flat, value; bool present; };
struct dev { int fd; enum kind kind; char node[32]; bool native; struct axis ax[4]; };
static struct dev devs[MAXDEV]; static int ndev;
static bool held[R_COUNT], paused, slow, alt_held;
static long long power_down_ms = -1;

static void open_axes(struct dev *d) {
    static const unsigned codes[4] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
    for (int i = 0; i < 4; i++) {
        struct input_absinfo a;
        if (ioctl(d->fd, EVIOCGABS(codes[i]), &a) == 0 && a.maximum > a.minimum) {
            d->ax[i] = (struct axis){a.minimum, a.maximum, a.flat, a.value, true};
        }
    }
}
static bool has_all_native(int fd) {
    unsigned long key[BITS(KEY_MAX+1)] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0) return false;
    for (int r = 0; r < R_COUNT; r++) if (!TEST(key, native_code[r])) return false;
    return true;
}
static void rescan(void) {
    DIR *d = opendir("/dev/input"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ndev < MAXDEV) {
        if (strncmp(e->d_name, "event", 5)) continue;
        bool known = false;
        for (int i = 0; i < ndev; i++) if (!strcmp(devs[i].node, e->d_name)) known = true;
        if (known) continue;
        char path[PATH_MAX]; snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        enum kind k = classify(fd);
        if (k != K_PAD && k != K_KEYS) { close(fd); continue; }
        struct dev *v = &devs[ndev++]; memset(v, 0, sizeof *v);
        v->fd = fd; v->kind = k; snprintf(v->node, sizeof v->node, "%s", e->d_name);
        if (k == K_PAD) { v->native = has_all_native(fd); open_axes(v); }
        fprintf(stderr, "a36-padmouse: using %s as %s%s\n", e->d_name, k == K_PAD ? "pad" : "keys",
                k == K_PAD ? (v->native ? " (native codes)" : " (legacy codes)") : "");
    }
    closedir(d);
}
static void drop_dev(int i) { close(devs[i].fd); devs[i] = devs[--ndev]; }

static int role_of(const struct dev *d, unsigned code) {
    const unsigned *table = d->native ? native_code : legacy_code;
    for (int r = 0; r < R_COUNT; r++) if (table[r] == code) return r;
    return -1;
}
static void pad_button(int r, bool down) {
    bool was = held[r]; held[r] = down;
    if (was == down) return;
    if (held[R_FN] && down) {                       /* Fn combinations */
        if (r == R_Y) { paused = !paused; fprintf(stderr, "a36-padmouse: %s\n", paused ? "paused" : "resumed"); }
        else if (r == R_UP) brightness(+1);
        else if (r == R_DOWN) brightness(-1);
        return;
    }
    if (paused || r == R_FN) return;
    static const unsigned plain[R_COUNT] = {
        [R_A] = BTN_LEFT, [R_B] = BTN_RIGHT, [R_X] = KEY_ENTER, [R_L2] = KEY_PAGEUP, [R_R2] = KEY_PAGEDOWN,
        [R_L3] = BTN_MIDDLE, [R_SELECT] = KEY_ESC, [R_START] = KEY_LEFTMETA,
        [R_UP] = KEY_UP, [R_DOWN] = KEY_DOWN, [R_LEFT] = KEY_LEFT, [R_RIGHT] = KEY_RIGHT};
    switch (r) {
    case R_Y: if (down) toggle_osk(); return;
    case R_R3: if (down) slow = !slow; return;
    case R_L1:                                       /* hold Alt, step with Tab */
        if (down) { emit(EV_KEY, KEY_LEFTALT, 1); alt_held = true; syn(); tap(KEY_TAB); }
        else if (alt_held) { emit(EV_KEY, KEY_LEFTALT, 0); alt_held = false; syn(); }
        return;
    case R_R1: if (down) tap(KEY_TAB); return;
    default:
        if (plain[r]) { emit(EV_KEY, plain[r], down); syn(); }
    }
}
static void key_event(unsigned code, int value) {
    if (code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN) {
        if (value == 0) return;
        if (held[R_FN]) brightness(code == KEY_VOLUMEUP ? +1 : -1);
        else run_helper("volume", code == KEY_VOLUMEUP ? "up" : "down");
    } else if (code == KEY_POWER) {
        if (value == 1) power_down_ms = now_ms();
        else if (value == 0 && power_down_ms >= 0) {
            if (now_ms() - power_down_ms < 1000) run_helper("sleep", "suspend");
            power_down_ms = -1;
        }
    }
}
static double norm(const struct axis *a) {
    if (!a->present) return 0;
    double mid = (a->min + a->max) / 2.0, half = (a->max - a->min) / 2.0;
    double v = (a->value - mid) / half, dead = 0.15;
    if (a->flat > 0 && a->flat / half > dead) dead = a->flat / half;
    if (v > -dead && v < dead) return 0;
    v = v > 0 ? (v - dead) / (1 - dead) : (v + dead) / (1 - dead);
    return v > 1 ? 1 : v < -1 ? -1 : v;
}
static void motion_tick(void) {
    static double fx, fy, sx, sy;
    if (paused) return;
    struct dev *pad = NULL;
    for (int i = 0; i < ndev; i++) if (devs[i].kind == K_PAD) { pad = &devs[i]; break; }
    if (!pad) return;
    double x = norm(&pad->ax[0]), y = norm(&pad->ax[1]), rx = norm(&pad->ax[2]), ry = norm(&pad->ax[3]);
    double speed = slow ? 3.0 : 10.0;               /* pixels per 10 ms at full deflection */
    fx += x * (x < 0 ? -x : x) * speed; fy += y * (y < 0 ? -y : y) * speed;
    sx += rx * 0.12; sy += ry * 0.12;              /* wheel steps per 10 ms at full deflection */
    int dx = (int)fx, dy = (int)fy, wx = (int)sx, wy = (int)sy;
    fx -= dx; fy -= dy; sx -= wx; sy -= wy;
    if (!x) fx = 0;
    if (!y) fy = 0;
    if (!rx) sx = 0;
    if (!ry) sy = 0;
    if (dx) emit(EV_REL, REL_X, dx);
    if (dy) emit(EV_REL, REL_Y, dy);
    if (wy) emit(EV_REL, REL_WHEEL, -wy);
    if (wx) emit(EV_REL, REL_HWHEEL, wx);
    if (dx || dy || wx || wy) syn();
}
static void handle(struct dev *d, const struct input_event *ev) {
    if (ev->type == EV_KEY) {
        if (d->kind == K_PAD) {
            int r = role_of(d, ev->code);
            if (r >= 0) { if (ev->value != 2) pad_button(r, ev->value != 0); return; }
        }
        key_event(ev->code, ev->value);
    } else if (ev->type == EV_ABS && d->kind == K_PAD) {
        static const unsigned codes[4] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
        for (int i = 0; i < 4; i++) if (ev->code == codes[i]) d->ax[i].value = ev->value;
        if (ev->code == ABS_HAT0X || ev->code == ABS_HAT0Y) {   /* pads that report the D-pad as a hat */
            int base = ev->code == ABS_HAT0X ? R_LEFT : R_UP;
            pad_button(base, ev->value < 0); pad_button(base + 1, ev->value > 0);
        }
    }
}

static int run(void) {
    struct sigaction sa = {.sa_handler = on_stop};
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    struct sigaction child = {.sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT};
    sigaction(SIGCHLD, &child, NULL);
    char event[32] = "";
    out_fd = uinput_create(event, sizeof event);
    if (out_fd < 0) return 1;
    unsigned major, minor;
    if (event[0] && !read_dev_number(event, &major, &minor))
        write_db(major, minor, "E:ID_INPUT_KEY=1\nE:ID_INPUT_KEYBOARD=1\nE:ID_INPUT_MOUSE=1\n", true);
    /* The host supervisor mirrors new nodes into this /dev within ~50 ms. */
    char node[PATH_MAX]; snprintf(node, sizeof node, "/dev/input/%s", event);
    for (int i = 0; i < 300 && event[0] && access(node, F_OK); i++) usleep(10000);
    char ready[PATH_MAX]; snprintf(ready, sizeof ready, "%s/padmouse.ready", state_dir);
    FILE *f = fopen(ready, "we");
    if (f) { fprintf(f, "%s\n", event[0] && !access(node, F_OK) ? event : "missing"); fclose(f); }
    long long next_scan = 0, next_tick = now_ms();
    while (!stop_flag) {
        long long t = now_ms();
        if (t >= next_scan) { rescan(); next_scan = t + 2000; }
        struct pollfd p[MAXDEV];
        for (int i = 0; i < ndev; i++) p[i] = (struct pollfd){.fd = devs[i].fd, .events = POLLIN};
        int wait = (int)(next_tick - t); if (wait < 0) wait = 0;
        int n = poll(p, ndev, wait);
        if (n < 0 && errno != EINTR) break;
        for (int i = ndev - 1; n > 0 && i >= 0; i--) {
            if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { drop_dev(i); continue; }
            if (!(p[i].revents & POLLIN)) continue;
            struct input_event ev; ssize_t r;
            while ((r = read(devs[i].fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) handle(&devs[i], &ev);
            if (r < 0 && errno != EAGAIN && errno != EINTR) drop_dev(i);
        }
        if (now_ms() >= next_tick) { motion_tick(); next_tick += 10; if (next_tick < now_ms()) next_tick = now_ms() + 10; }
    }
    if (alt_held) { emit(EV_KEY, KEY_LEFTALT, 0); syn(); }
    ioctl(out_fd, UI_DEV_DESTROY); close(out_fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "seed-udev")) { seed_existing(); return 0; }
    if (argc == 3 && !strcmp(argv[1], "run")) { state_dir = argv[2]; return run(); }
    fprintf(stderr, "usage: a36-padmouse seed-udev | run STATE_DIR\n");
    return 2;
}
