/* ===========================================================================
 * PumpkinOS - graphical desktop
 * ---------------------------------------------------------------------------
 * A small compositor over the mode-13h back buffer: a teal desktop, a taskbar
 * with a live clock, and a couple of windows you can drag by their title bars,
 * raise by clicking, and close with the [x] button. The mouse drives an arrow
 * cursor; Esc returns to the shell.
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
    COL_DESKTOP,        /* 1  */
    COL_FACE,           /* 2  window body            */
    COL_TEXT,           /* 3  window text            */
    COL_TITLE,          /* 4  active title bar       */
    COL_TITLE_TEXT,     /* 5                          */
    COL_BORDER,         /* 6                          */
    COL_TASKBAR,        /* 7                          */
    COL_TASK_TEXT,      /* 8                          */
    COL_CLOSE,          /* 9  close button           */
    COL_SHADOW,         /* 10                         */
    COL_ACCENT,         /* 11                         */
    COL_INACTIVE,       /* 12 inactive title bar     */
    COL_WHITE = 15,
};

static void load_palette(void) {
    vga_set_palette(COL_BLACK,       0,  0,  0);
    vga_set_palette(COL_DESKTOP,     8, 26, 34);
    vga_set_palette(COL_FACE,       50, 50, 55);
    vga_set_palette(COL_TEXT,        4,  4,  6);
    vga_set_palette(COL_TITLE,      12, 22, 55);
    vga_set_palette(COL_TITLE_TEXT, 62, 62, 63);
    vga_set_palette(COL_BORDER,      5,  5,  9);
    vga_set_palette(COL_TASKBAR,    18, 18, 27);
    vga_set_palette(COL_TASK_TEXT,  56, 56, 61);
    vga_set_palette(COL_CLOSE,      54, 22, 22);
    vga_set_palette(COL_SHADOW,      3, 11, 15);
    vga_set_palette(COL_ACCENT,     32, 44, 90);
    vga_set_palette(COL_INACTIVE,   30, 30, 38);
    vga_set_palette(COL_WHITE,      63, 63, 63);
}

/* ---- windows ------------------------------------------------------------- */
#define TITLE_H 14
#define MAX_WIN 2

struct window {
    int  x, y, w, h;
    int  open;
    const char *title;
    const char *body[4];
    int  nbody;
};

static struct window wins[MAX_WIN];
static int  zorder[MAX_WIN];        /* back-to-front draw order (indices) */

static void windows_init(void) {
    wins[0] = (struct window){
        .x = 20, .y = 26, .w = 186, .h = 92, .open = 1,
        .title = "Welcome",
        .body = { "A 32-bit hobby OS,",
                  "in VGA 320x200 mode.",
                  "Drag my title bar;",
                  "click [x] to close." },
        .nbody = 4,
    };
    wins[1] = (struct window){
        .x = 150, .y = 100, .w = 150, .h = 62, .open = 1,
        .title = "About",
        .body = { "Mouse + keyboard.",
                  "Press Esc to exit." },
        .nbody = 2,
    };
    zorder[0] = 0;
    zorder[1] = 1;
}

/* Move window 'idx' to the top of the z-order. */
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

    gfx_fill_rect(x + 3, y + 3, w, h, COL_SHADOW);      /* drop shadow */
    gfx_fill_rect(x, y, w, h, COL_FACE);
    gfx_rect(x, y, w, h, COL_BORDER);

    gfx_fill_rect(x + 1, y + 1, w - 2, TITLE_H, active ? COL_TITLE : COL_INACTIVE);
    gfx_text(x + 5, y + 2, win->title, COL_TITLE_TEXT, -1);

    int cb = close_box_x(win);
    gfx_fill_rect(cb, y + 2, TITLE_H - 2, TITLE_H - 2, COL_CLOSE);
    gfx_char(cb + 2, y + 1, 'x', COL_TITLE_TEXT, -1);

    for (int i = 0; i < win->nbody; i++)
        gfx_text(x + 6, y + TITLE_H + 5 + i * (FONT_H - 2),
                 win->body[i], COL_TEXT, -1);
}

/* ---- mouse cursor (arrow) ------------------------------------------------- */
static const char *const cursor_bmp[] = {
    "#",         "##",        "#.#",       "#..#",
    "#...#",     "#....#",    "#.....#",   "#......#",
    "#...####",  "#..#",      "#.# #",     "## ##",
    0,
};

static void draw_cursor(int mx, int my) {
    for (int row = 0; cursor_bmp[row]; row++) {
        const char *line = cursor_bmp[row];
        for (int col = 0; line[col]; col++) {
            if (line[col] == '#')
                gfx_pixel(mx + col, my + row, COL_BLACK);
            else if (line[col] == '.')
                gfx_pixel(mx + col, my + row, COL_WHITE);
        }
    }
}

/* ---- taskbar + clock ------------------------------------------------------ */
static void two(char *p, uint32_t v) {
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}

static void draw_taskbar(void) {
    int y = GFX_H - 16;
    gfx_fill_rect(0, y, GFX_W, 16, COL_TASKBAR);
    gfx_hline(0, y, GFX_W, COL_ACCENT);

    gfx_fill_rect(3, y + 2, 74, 12, COL_ACCENT);
    gfx_text(8, y, "PumpkinOS", COL_TITLE_TEXT, -1);

    struct rtc_time t;
    rtc_read(&t);
    char clock[9];
    two(clock, t.hour);       clock[2] = ':';
    two(clock + 3, t.minute); clock[5] = ':';
    two(clock + 6, t.second); clock[8] = '\0';
    gfx_text(GFX_W - gfx_text_width(clock) - 6, y, clock, COL_TASK_TEXT, -1);
}

/* ---- compositor ----------------------------------------------------------- */
static void redraw(int mx, int my) {
    gfx_clear(COL_DESKTOP);
    for (int i = 0; i < MAX_WIN; i++) {
        int idx = zorder[i];
        if (wins[idx].open)
            draw_window(&wins[idx], i == MAX_WIN - 1);   /* top-most is active */
    }
    draw_taskbar();
    draw_cursor(mx, my);
    gfx_present();
}

/* ---- hit testing ---------------------------------------------------------- */
static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ---- event loop ----------------------------------------------------------- */
void desktop_run(void) {
    vga_enter_graphics();
    load_palette();
    windows_init();

    int mx = mouse_x(), my = mouse_y();
    int prev_buttons = 0;
    int dragging = -1, grab_dx = 0, grab_dy = 0;   /* window being dragged */

    uint32_t last_seq = mouse_seq() - 1;           /* force first draw */
    struct rtc_time last_clock;
    rtc_read(&last_clock);

    for (;;) {
        if (keyboard_haschar()) {
            if (keyboard_getchar() == 27)          /* Esc */
                break;
        }

        int changed = 0;

        uint32_t seq = mouse_seq();
        if (seq != last_seq) {
            last_seq = seq;
            mx = mouse_x();
            my = mouse_y();
            int buttons = mouse_buttons();
            int left = buttons & MOUSE_LEFT;
            int was_left = prev_buttons & MOUSE_LEFT;

            if (left && !was_left) {               /* press: hit-test front->back */
                for (int i = MAX_WIN - 1; i >= 0; i--) {
                    int idx = zorder[i];
                    struct window *w = &wins[idx];
                    if (!w->open)
                        continue;
                    if (in_rect(mx, my, w->x, w->y, w->w, w->h) ||
                        in_rect(mx, my, w->x, w->y, w->w, TITLE_H + 1)) {
                        raise_window(idx);
                        if (in_rect(mx, my, close_box_x(w), w->y + 2,
                                    TITLE_H - 2, TITLE_H - 2)) {
                            w->open = 0;           /* clicked [x] */
                        } else if (in_rect(mx, my, w->x, w->y, w->w, TITLE_H + 1)) {
                            dragging = idx;        /* grab the title bar */
                            grab_dx = mx - w->x;
                            grab_dy = my - w->y;
                        }
                        break;
                    }
                }
            } else if (!left && was_left) {
                dragging = -1;                     /* release */
            }

            if (dragging >= 0 && left) {           /* follow the cursor */
                struct window *w = &wins[dragging];
                int nx = mx - grab_dx, ny = my - grab_dy;
                if (nx < -(w->w - 40)) nx = -(w->w - 40);
                if (nx > GFX_W - 40)   nx = GFX_W - 40;
                if (ny < 0) ny = 0;
                if (ny > GFX_H - 16 - TITLE_H) ny = GFX_H - 16 - TITLE_H;
                w->x = nx;
                w->y = ny;
            }

            prev_buttons = buttons;
            changed = 1;
        }

        struct rtc_time now;
        rtc_read(&now);
        if (now.second != last_clock.second) {
            last_clock = now;
            changed = 1;
        }

        if (changed)
            redraw(mx, my);

        __asm__ volatile("sti; hlt");
    }

    vga_leave_graphics();
}
