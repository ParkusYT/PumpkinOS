/* ===========================================================================
 * PumpkinOS - graphical desktop
 * ---------------------------------------------------------------------------
 * A small compositor over the framebuffer: a desktop, a taskbar with a live
 * clock, and windows you can drag by their title bars, raise by clicking, and
 * close with the [x] button. Adapts to whatever resolution the VGA/VBE driver
 * gives us. Esc returns to the shell.
 * ========================================================================= */
#include "desktop.h"
#include "vga.h"
#include "gfx.h"
#include "keyboard.h"
#include "mouse.h"
#include "rtc.h"
#include <stdint.h>

/* ---- palette ------------------------------------------------------------- */
enum {
    COL_BLACK = 0,
    COL_DESKTOP, COL_FACE, COL_TEXT, COL_TITLE, COL_TITLE_TEXT, COL_BORDER,
    COL_TASKBAR, COL_TASK_TEXT, COL_CLOSE, COL_SHADOW, COL_ACCENT, COL_INACTIVE,
    COL_WHITE = 15,
};

static void load_palette(void) {
    gfx_set_palette(COL_BLACK,       0,  0,  0);
    gfx_set_palette(COL_DESKTOP,     8, 26, 34);
    gfx_set_palette(COL_FACE,       50, 50, 55);
    gfx_set_palette(COL_TEXT,        4,  4,  6);
    gfx_set_palette(COL_TITLE,      12, 22, 55);
    gfx_set_palette(COL_TITLE_TEXT, 62, 62, 63);
    gfx_set_palette(COL_BORDER,      5,  5,  9);
    gfx_set_palette(COL_TASKBAR,    18, 18, 27);
    gfx_set_palette(COL_TASK_TEXT,  56, 56, 61);
    gfx_set_palette(COL_CLOSE,      54, 22, 22);
    gfx_set_palette(COL_SHADOW,      3, 11, 15);
    gfx_set_palette(COL_ACCENT,     32, 44, 90);
    gfx_set_palette(COL_INACTIVE,   30, 30, 38);
    gfx_set_palette(COL_WHITE,      63, 63, 63);
}

/* ---- layout metrics (depend on resolution) -------------------------------- */
#define TITLE_H 18
#define TB_H    22

static int W, H;                    /* screen size */
static int cur_scale;               /* cursor magnification */

/* ---- windows ------------------------------------------------------------- */
#define MAX_WIN 2

struct window {
    int  x, y, w, h;
    int  open;
    const char *title;
    const char *body[4];
    int  nbody;
};

static struct window wins[MAX_WIN];
static int  zorder[MAX_WIN];

static void windows_init(void) {
    int big = (W >= 640);
    int w0 = big ? 340 : 186, h0 = big ? 150 : 92;
    int w1 = big ? 300 : 150, h1 = big ? 120 : 62;

    if (big) {
        wins[0] = (struct window){
            .x = W / 6, .y = H / 7, .w = w0, .h = h0, .open = 1,
            .title = "Welcome to PumpkinOS",
            .body = { "A 32-bit BIOS-only hobby OS.",
                      "This desktop runs on a linear",
                      "framebuffer. Drag my title bar,",
                      "click [x] to close." },
            .nbody = 4,
        };
        wins[1] = (struct window){
            .x = W / 2, .y = H / 2 - 20, .w = w1, .h = h1, .open = 1,
            .title = "About",
            .body = { "Mouse + keyboard driven.",
                      "Press Esc to exit." },
            .nbody = 2,
        };
    } else {                            /* compact layout for 320x200 */
        wins[0] = (struct window){
            .x = W / 6, .y = H / 7, .w = w0, .h = h0, .open = 1,
            .title = "Welcome",
            .body = { "A 32-bit hobby OS,",
                      "on a framebuffer.",
                      "Drag my title bar;",
                      "click [x] to close." },
            .nbody = 4,
        };
        wins[1] = (struct window){
            .x = W / 2, .y = H / 2 - 20, .w = w1, .h = h1, .open = 1,
            .title = "About",
            .body = { "Mouse + keyboard.",
                      "Press Esc to exit." },
            .nbody = 2,
        };
    }
    zorder[0] = 0;
    zorder[1] = 1;
}

static void raise_window(int idx) {
    int at = 0;
    while (at < MAX_WIN && zorder[at] != idx) at++;
    for (; at < MAX_WIN - 1; at++)
        zorder[at] = zorder[at + 1];
    zorder[MAX_WIN - 1] = idx;
}

static int close_box_x(const struct window *w) { return w->x + w->w - TITLE_H - 1; }

static void draw_window(const struct window *win, int active) {
    int x = win->x, y = win->y, w = win->w, h = win->h;

    gfx_fill_rect(x + 4, y + 4, w, h, COL_SHADOW);
    gfx_fill_rect(x, y, w, h, COL_FACE);
    gfx_rect(x, y, w, h, COL_BORDER);

    gfx_fill_rect(x + 1, y + 1, w - 2, TITLE_H, active ? COL_TITLE : COL_INACTIVE);
    gfx_text(x + 6, y + 2, win->title, COL_TITLE_TEXT, -1);

    int cb = close_box_x(win);
    gfx_fill_rect(cb, y + 2, TITLE_H - 3, TITLE_H - 3, COL_CLOSE);
    gfx_char(cb + 2, y + 1, 'x', COL_TITLE_TEXT, -1);

    for (int i = 0; i < win->nbody; i++)
        gfx_text(x + 8, y + TITLE_H + 6 + i * (FONT_H + 2), win->body[i], COL_TEXT, -1);
}

/* ---- mouse cursor (arrow) ------------------------------------------------- */
static const char *const cursor_bmp[] = {
    "#",         "##",        "#.#",       "#..#",
    "#...#",     "#....#",    "#.....#",   "#......#",
    "#...####",  "#..#",      "#.# #",     "## ##",
    0,
};

#define CUR_COLS 8
#define CUR_ROWS 12

/* Paint the cursor straight onto the framebuffer (over the composed scene),
 * so moving it never touches the back buffer or triggers a full blit. */
static void draw_cursor_fb(int mx, int my) {
    int s = cur_scale;
    for (int row = 0; cursor_bmp[row]; row++) {
        const char *line = cursor_bmp[row];
        for (int col = 0; line[col]; col++) {
            uint8_t c;
            if (line[col] == '#')      c = COL_BLACK;
            else if (line[col] == '.') c = COL_WHITE;
            else continue;
            for (int sy = 0; sy < s; sy++)
                for (int sx = 0; sx < s; sx++)
                    gfx_fb_pixel(mx + col * s + sx, my + row * s + sy, c);
        }
    }
}

/* ---- taskbar + clock ------------------------------------------------------ */
static void two(char *p, uint32_t v) {
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}

/* Append an unsigned integer to a string; returns the new end pointer. */
static char *append_uint(char *p, uint32_t v) {
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static void draw_taskbar(void) {
    int y = H - TB_H;
    gfx_fill_rect(0, y, W, TB_H, COL_TASKBAR);
    gfx_hline(0, y, W, COL_ACCENT);

    int pad = (TB_H - FONT_H) / 2;
    gfx_fill_rect(4, y + 2, 9 * FONT_W + 8, TB_H - 4, COL_ACCENT);
    gfx_text(8, y + pad, "PumpkinOS", COL_TITLE_TEXT, -1);

    /* show the active resolution (e.g. "1024x768x32") */
    char mode[20]; char *p = mode;
    p = append_uint(p, (uint32_t)W); *p++ = 'x';
    p = append_uint(p, (uint32_t)H); *p++ = 'x';
    p = append_uint(p, (uint32_t)vga_bpp()); *p = 0;
    gfx_text(9 * FONT_W + 24, y + pad, mode, COL_TASK_TEXT, -1);

    struct rtc_time t;
    rtc_read(&t);
    char clock[9];
    two(clock, t.hour);       clock[2] = ':';
    two(clock + 3, t.minute); clock[5] = ':';
    two(clock + 6, t.second); clock[8] = '\0';
    gfx_text(W - gfx_text_width(clock) - 8, y + pad, clock, COL_TASK_TEXT, -1);
}

/* ---- compositor ----------------------------------------------------------- */
/* Compose the whole scene into the back buffer (no cursor - that goes straight
 * to the framebuffer afterwards). */
static void render_scene(void) {
    gfx_clear(COL_DESKTOP);
    for (int i = 0; i < MAX_WIN; i++) {
        int idx = zorder[i];
        if (wins[idx].open)
            draw_window(&wins[idx], i == MAX_WIN - 1);
    }
    draw_taskbar();
}

static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ---- event loop ----------------------------------------------------------- */
void desktop_run(void) {
    vga_enter_graphics();
    if (gfx_init() != 0) {           /* out of memory for the back buffer */
        vga_leave_graphics();
        return;
    }
    W = gfx_width();
    H = gfx_height();
    cur_scale = (W >= 640) ? 2 : 1;
    load_palette();
    windows_init();
    mouse_set_bounds(W, H);

    int cw = CUR_COLS * cur_scale, ch = CUR_ROWS * cur_scale;

    int mx = mouse_x(), my = mouse_y();
    int cx = mx, cy = my;                 /* where the cursor is drawn now */
    int prev_buttons = 0;
    int dragging = -1, grab_dx = 0, grab_dy = 0;

    uint32_t last_seq = mouse_seq() - 1;
    struct rtc_time last_clock;
    rtc_read(&last_clock);

    /* Compose once and show it, then paint the cursor on top. */
    render_scene();
    gfx_present();
    draw_cursor_fb(mx, my);
    cx = mx; cy = my;

    for (;;) {
        if (keyboard_haschar()) {
            if (keyboard_getchar() == 27)
                break;
        }

        int scene_full = 0;               /* raise/close -> full blit           */
        int scene_taskbar = 0;            /* clock ticked -> blit taskbar only  */
        int dirty = 0, dx0 = 0, dy0 = 0, dw = 0, dh = 0;  /* drag -> blit region */

        uint32_t seq = mouse_seq();
        if (seq != last_seq) {
            last_seq = seq;
            mx = mouse_x();
            my = mouse_y();
            int buttons = mouse_buttons();
            int left = buttons & MOUSE_LEFT;
            int was_left = prev_buttons & MOUSE_LEFT;

            if (left && !was_left) {
                for (int i = MAX_WIN - 1; i >= 0; i--) {
                    int idx = zorder[i];
                    struct window *w = &wins[idx];
                    if (!w->open || !in_rect(mx, my, w->x, w->y, w->w, w->h))
                        continue;
                    raise_window(idx);
                    scene_full = 1;                    /* raise reorders windows */
                    if (in_rect(mx, my, close_box_x(w), w->y + 2,
                                TITLE_H - 3, TITLE_H - 3)) {
                        w->open = 0;
                    } else if (in_rect(mx, my, w->x, w->y, w->w, TITLE_H + 1)) {
                        dragging = idx;
                        grab_dx = mx - w->x;
                        grab_dy = my - w->y;
                    }
                    break;
                }
            } else if (!left && was_left) {
                dragging = -1;
            }

            if (dragging >= 0 && left) {
                struct window *w = &wins[dragging];
                int ox = w->x, oy = w->y;
                int nx = mx - grab_dx, ny = my - grab_dy;
                if (nx < -(w->w - 48)) nx = -(w->w - 48);
                if (nx > W - 48)       nx = W - 48;
                if (ny < 0) ny = 0;
                if (ny > H - TB_H - TITLE_H) ny = H - TB_H - TITLE_H;
                if (nx != ox || ny != oy) {
                    w->x = nx;
                    w->y = ny;
                    /* dirty region = old + new window box (+ border/shadow) */
                    int lo_x = (ox < nx ? ox : nx) - 1;
                    int lo_y = (oy < ny ? oy : ny) - 1;
                    int hi_x = (ox > nx ? ox : nx) + w->w + 6;
                    int hi_y = (oy > ny ? oy : ny) + w->h + 6;
                    dirty = 1;
                    dx0 = lo_x; dy0 = lo_y; dw = hi_x - lo_x; dh = hi_y - lo_y;
                }
            }

            prev_buttons = buttons;
        }

        struct rtc_time now;
        rtc_read(&now);
        if (now.second != last_clock.second) {
            last_clock = now;
            scene_taskbar = 1;
        }

        if (scene_full) {
            /* full recompose + blit; the blit also wipes the old cursor */
            render_scene();
            gfx_present();
            draw_cursor_fb(mx, my);
            cx = mx; cy = my;
        } else {
            if (dirty) {                      /* dragging: recompose, blit region */
                render_scene();
                gfx_present_rect(dx0, dy0, dw, dh);
            } else if (scene_taskbar) {
                render_scene();
                gfx_present_rect(0, H - TB_H, W, TB_H);
                if (cy + ch > H - TB_H)       /* cursor overlapped the taskbar */
                    draw_cursor_fb(cx, cy);
            }
            if (dirty || mx != cx || my != cy) {   /* move cursor: save-under */
                gfx_present_rect(cx, cy, cw, ch);   /* restore scene under old */
                draw_cursor_fb(mx, my);
                cx = mx; cy = my;
            }
        }

        __asm__ volatile("sti; hlt");
    }

    gfx_shutdown();
    vga_leave_graphics();
}
