/* ===========================================================================
 * PumpkinOS - graphical desktop
 * ---------------------------------------------------------------------------
 * A small compositor over the framebuffer: a desktop that shows the files in
 * the root directory as clickable icons, draggable windows, and a taskbar with
 * a Start menu (Enter shell / Reboot / Power Off) and a live clock. Adapts to
 * whatever resolution the VGA/VBE driver gives us. Esc returns to the shell.
 * ========================================================================= */
#include "desktop.h"
#include "vga.h"
#include "gfx.h"
#include "keyboard.h"
#include "mouse.h"
#include "rtc.h"
#include "acpi.h"
#include "fat12.h"
#include "io.h"
#include "string.h"
#include <stdint.h>

/* ---- palette ------------------------------------------------------------- */
enum {
    COL_BLACK = 0,
    COL_DESKTOP, COL_FACE, COL_TEXT, COL_TITLE, COL_TITLE_TEXT, COL_BORDER,
    COL_TASKBAR, COL_TASK_TEXT, COL_CLOSE, COL_SHADOW, COL_ACCENT, COL_INACTIVE,
    COL_WHITE = 15,
    COL_FOLDER, COL_FOLDER_D, COL_PAGE, COL_PAGE_LINE, COL_LABEL, COL_SELECT,
    COL_MENU, COL_MENU_HI, COL_MENU_TEXT, COL_START, COL_START_HI,
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
    gfx_set_palette(COL_ACCENT,     32, 44, 63);
    gfx_set_palette(COL_INACTIVE,   30, 30, 38);
    gfx_set_palette(COL_WHITE,      63, 63, 63);
    gfx_set_palette(COL_FOLDER,     60, 50, 22);
    gfx_set_palette(COL_FOLDER_D,   40, 32, 10);
    gfx_set_palette(COL_PAGE,       60, 60, 62);
    gfx_set_palette(COL_PAGE_LINE,  34, 34, 42);
    gfx_set_palette(COL_LABEL,      58, 58, 62);
    gfx_set_palette(COL_SELECT,     26, 40, 62);
    gfx_set_palette(COL_MENU,       28, 28, 40);
    gfx_set_palette(COL_MENU_HI,    38, 50, 63);
    gfx_set_palette(COL_MENU_TEXT,  60, 60, 63);
    gfx_set_palette(COL_START,      22, 40, 52);
    gfx_set_palette(COL_START_HI,   36, 54, 63);
}

/* ---- layout metrics (depend on resolution) -------------------------------- */
#define TITLE_H 18
#define TB_H    22

static int W, H;                    /* screen size */
static int big;                     /* hi-res layout? */
static int cur_scale;               /* cursor magnification */

/* ---- string helpers ------------------------------------------------------- */
static void set_str(char *d, const char *s, int cap) {
    int i = 0;
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = '\0';
}
static char *append_uint(char *p, uint32_t v) {
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

/* ---- root-directory icons ------------------------------------------------- */
#define MAX_ICONS 32
static struct fs_dirent entries[MAX_ICONS];
static int nentries;
static int selected = -1;

static int cellw, cellh, icon_pad, icon_cols, glyph_w, glyph_h;

static void layout_icons(void) {
    cellw   = big ? 88 : 52;
    cellh   = big ? 56 : 42;
    icon_pad = big ? 16 : 6;
    glyph_w = big ? 34 : 20;
    glyph_h = big ? 26 : 16;
    icon_cols = (W - icon_pad * 2) / cellw;
    if (icon_cols < 1) icon_cols = 1;
}

static void icon_pos(int idx, int *ix, int *iy) {
    *ix = icon_pad + (idx % icon_cols) * cellw;
    *iy = icon_pad + (idx / icon_cols) * cellh;
}

static void draw_icon(int idx) {
    int ix, iy;
    icon_pos(idx, &ix, &iy);
    const struct fs_dirent *e = &entries[idx];

    if (idx == selected)
        gfx_fill_rect(ix + 2, iy, cellw - 4, cellh - 2, COL_SELECT);

    int gx = ix + (cellw - glyph_w) / 2;
    int gy = iy + 3;

    if (e->is_dir) {
        int tab = glyph_h / 3;
        gfx_fill_rect(gx, gy, glyph_w / 2, tab, COL_FOLDER_D);   /* tab */
        gfx_fill_rect(gx, gy + tab / 2, glyph_w, glyph_h - tab / 2, COL_FOLDER);
        gfx_rect(gx, gy + tab / 2, glyph_w, glyph_h - tab / 2, COL_FOLDER_D);
    } else {
        gfx_fill_rect(gx, gy, glyph_w, glyph_h, COL_PAGE);
        gfx_rect(gx, gy, glyph_w, glyph_h, COL_PAGE_LINE);
        for (int i = 1; i <= 3; i++)                             /* text lines */
            gfx_hline(gx + 3, gy + i * (glyph_h / 4), glyph_w - 6, COL_PAGE_LINE);
    }

    /* label: centred under the glyph, truncated to fit the cell */
    char label[13];
    set_str(label, e->name, sizeof(label));
    int maxc = (cellw - 2) / FONT_W;
    if (maxc < 1) maxc = 1;
    if ((int)strlen(label) > maxc) label[maxc] = '\0';
    int tw = gfx_text_width(label);
    gfx_text(ix + (cellw - tw) / 2, iy + glyph_h + 6, label, COL_LABEL, -1);
}

static int icon_at(int px, int py) {
    for (int i = 0; i < nentries; i++) {
        int ix, iy;
        icon_pos(i, &ix, &iy);
        if (px >= ix && px < ix + cellw && py >= iy && py < iy + cellh)
            return i;
    }
    return -1;
}

/* ---- windows ------------------------------------------------------------- */
#define MAX_WIN    2
#define WIN_WELCOME 0
#define WIN_INFO    1
#define BODY_MAX   5
#define LINE_C     40

struct window {
    int  x, y, w, h, open;
    char title[28];
    char body[BODY_MAX][LINE_C];
    int  nbody;
};

static struct window wins[MAX_WIN];
static int zorder[MAX_WIN];

static void windows_init(void) {
    struct window *w = &wins[WIN_WELCOME];
    w->w = big ? 300 : 188;
    w->h = big ? 128 : 96;
    w->x = W / 2 - w->w / 2;
    w->y = big ? 40 : 18;
    w->open = 1;
    set_str(w->title, "Welcome to PumpkinOS", sizeof(w->title));
    set_str(w->body[0], "Files in / show as icons.",   LINE_C);
    set_str(w->body[1], "Click one to see details.",   LINE_C);
    set_str(w->body[2], "Click Start: shell / power.", LINE_C);
    set_str(w->body[3], "Drag title bars; [x] closes.", LINE_C);
    set_str(w->body[4], "Esc returns to the shell.",   LINE_C);
    w->nbody = 5;

    wins[WIN_INFO].open = 0;

    zorder[0] = WIN_WELCOME;
    zorder[1] = WIN_INFO;
}

static void open_info(int idx) {
    const struct fs_dirent *e = &entries[idx];
    struct window *w = &wins[WIN_INFO];
    set_str(w->title, e->name, sizeof(w->title));

    set_str(w->body[0], e->is_dir ? "Type:  directory" : "Type:  file", LINE_C);
    if (e->is_dir) {
        w->nbody = 1;
    } else {
        char line[LINE_C];
        char *p = line;
        *p++ = 'S'; *p++ = 'i'; *p++ = 'z'; *p++ = 'e'; *p++ = ':'; *p++ = ' '; *p++ = ' ';
        p = append_uint(p, e->size);
        *p++ = ' '; *p++ = 'b'; *p++ = 'y'; *p++ = 't'; *p++ = 'e'; *p++ = 's'; *p = '\0';
        set_str(w->body[1], line, LINE_C);
        w->nbody = 2;
    }

    w->w = big ? 220 : 160;
    w->h = big ? 84 : 68;
    w->x = W / 2 - w->w / 2;
    w->y = H / 3;
    w->open = 1;
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
        gfx_text(x + 10, y + TITLE_H + 8 + i * (FONT_H + 2), win->body[i], COL_TEXT, -1);
}

/* ---- taskbar + Start menu ------------------------------------------------- */
static int sb_x, sb_y, sb_w, sb_h;                  /* Start button */
static int mnu_x, mnu_y, mnu_w, mnu_h, mnu_ih;      /* Start menu */
static int start_open;
static int start_hover = -1;

static const char *const menu_items[3] = { "Enter shell", "Reboot", "Power Off" };

static void layout_taskbar(void) {
    sb_x = 4; sb_y = H - TB_H + 2; sb_w = big ? 66 : 48; sb_h = TB_H - 4;
    mnu_ih = FONT_H + 6;
    mnu_w  = big ? 150 : 118;
    mnu_h  = 3 * mnu_ih + 4;
    mnu_x  = 4;
    mnu_y  = H - TB_H - mnu_h;
}

static int menu_item_at(int px, int py) {
    if (!start_open) return -1;
    if (px < mnu_x || px >= mnu_x + mnu_w) return -1;
    if (py < mnu_y + 2 || py >= mnu_y + 2 + 3 * mnu_ih) return -1;
    return (py - (mnu_y + 2)) / mnu_ih;
}

static void two(char *p, uint32_t v) {
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}

static void draw_taskbar(void) {
    int y = H - TB_H;
    gfx_fill_rect(0, y, W, TB_H, COL_TASKBAR);
    gfx_hline(0, y, W, COL_ACCENT);

    int pad = (TB_H - FONT_H) / 2;
    gfx_fill_rect(sb_x, sb_y, sb_w, sb_h, start_open ? COL_START_HI : COL_START);
    gfx_rect(sb_x, sb_y, sb_w, sb_h, COL_BORDER);
    gfx_text(sb_x + (big ? 12 : 6), y + pad, "Start", COL_TITLE_TEXT, -1);

    /* active resolution (e.g. "1024x768x32") */
    char mode[20]; char *p = mode;
    p = append_uint(p, (uint32_t)W); *p++ = 'x';
    p = append_uint(p, (uint32_t)H); *p++ = 'x';
    p = append_uint(p, (uint32_t)vga_bpp()); *p = 0;
    if (big)
        gfx_text(sb_x + sb_w + 12, y + pad, mode, COL_TASK_TEXT, -1);

    struct rtc_time t;
    rtc_read(&t);
    char clock[9];
    two(clock, t.hour);       clock[2] = ':';
    two(clock + 3, t.minute); clock[5] = ':';
    two(clock + 6, t.second); clock[8] = '\0';
    gfx_text(W - gfx_text_width(clock) - 8, y + pad, clock, COL_TASK_TEXT, -1);
}

static void draw_start_menu(void) {
    if (!start_open) return;
    gfx_fill_rect(mnu_x + 3, mnu_y + 3, mnu_w, mnu_h, COL_SHADOW);
    gfx_fill_rect(mnu_x, mnu_y, mnu_w, mnu_h, COL_MENU);
    gfx_rect(mnu_x, mnu_y, mnu_w, mnu_h, COL_BORDER);
    for (int i = 0; i < 3; i++) {
        int iy = mnu_y + 2 + i * mnu_ih;
        if (i == start_hover)
            gfx_fill_rect(mnu_x + 2, iy, mnu_w - 4, mnu_ih, COL_MENU_HI);
        gfx_text(mnu_x + 10, iy + 3, menu_items[i],
                 i == start_hover ? COL_TITLE : COL_MENU_TEXT, -1);
    }
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

/* ---- compositor ----------------------------------------------------------- */
static void render_scene(void) {
    gfx_clear(COL_DESKTOP);
    for (int i = 0; i < nentries; i++)
        draw_icon(i);
    for (int i = 0; i < MAX_WIN; i++) {
        int idx = zorder[i];
        if (wins[idx].open)
            draw_window(&wins[idx], i == MAX_WIN - 1);
    }
    draw_taskbar();
    draw_start_menu();
}

static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ---- click handling ------------------------------------------------------- */
enum { ACT_NONE, ACT_SCENE, ACT_EXIT, ACT_REBOOT, ACT_POWEROFF };

/* set by a title-bar grab so the move handler drags the window */
static int dragging = -1, grab_dx, grab_dy;

static int handle_press(int mx, int my) {
    /* Start menu has priority: any click while it is open dismisses it. */
    if (start_open) {
        int mi = menu_item_at(mx, my);
        start_open = 0;
        start_hover = -1;
        if (mi == 0) return ACT_EXIT;
        if (mi == 1) return ACT_REBOOT;
        if (mi == 2) return ACT_POWEROFF;
        return ACT_SCENE;
    }

    if (in_rect(mx, my, sb_x, sb_y, sb_w, sb_h)) {
        start_open = 1;
        return ACT_SCENE;
    }

    /* windows, top of the z-order first */
    for (int i = MAX_WIN - 1; i >= 0; i--) {
        int idx = zorder[i];
        struct window *w = &wins[idx];
        if (!w->open || !in_rect(mx, my, w->x, w->y, w->w, w->h))
            continue;
        raise_window(idx);
        if (in_rect(mx, my, close_box_x(w), w->y + 2, TITLE_H - 3, TITLE_H - 3)) {
            w->open = 0;
        } else if (in_rect(mx, my, w->x, w->y, w->w, TITLE_H + 1)) {
            dragging = idx;
            grab_dx = mx - w->x;
            grab_dy = my - w->y;
        }
        return ACT_SCENE;
    }

    /* desktop icons */
    int ic = icon_at(mx, my);
    if (ic >= 0) {
        selected = ic;
        open_info(ic);
        raise_window(WIN_INFO);
        return ACT_SCENE;
    }

    if (selected != -1) { selected = -1; return ACT_SCENE; }
    return ACT_NONE;
}

/* ---- terminal actions ----------------------------------------------------- */
static void leave_graphics(void) {
    gfx_shutdown();
    vga_leave_graphics();
}
static void do_reboot(void) {
    leave_graphics();
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);                       /* pulse CPU reset via the 8042 */
    for (;;) __asm__ volatile("cli; hlt");
}
static void do_poweroff(void) {
    leave_graphics();
    acpi_poweroff();                        /* returns only if it failed */
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---- event loop ----------------------------------------------------------- */
void desktop_run(void) {
    vga_enter_graphics();
    if (gfx_init() != 0) {                  /* out of memory for the back buffer */
        vga_leave_graphics();
        return;
    }
    W = gfx_width();
    H = gfx_height();
    big = (W >= 640);
    cur_scale = big ? 2 : 1;

    nentries = fs_readdir_root(entries, MAX_ICONS);
    selected = -1;
    start_open = 0;
    dragging = -1;

    load_palette();
    layout_icons();
    layout_taskbar();
    windows_init();
    mouse_set_bounds(W, H);

    int cw = CUR_COLS * cur_scale, ch = CUR_ROWS * cur_scale;
    int mx = mouse_x(), my = mouse_y();
    int cx = mx, cy = my;
    int prev_buttons = 0;

    uint32_t last_seq = mouse_seq() - 1;
    struct rtc_time last_clock;
    rtc_read(&last_clock);

    render_scene();
    gfx_present();
    draw_cursor_fb(mx, my);
    cx = mx; cy = my;

    for (;;) {
        if (keyboard_haschar()) {
            if (keyboard_getchar() == 27)   /* Esc -> shell */
                break;
        }

        int scene_full = 0, scene_taskbar = 0;
        int dirty = 0, dx0 = 0, dy0 = 0, dw = 0, dh = 0;

        uint32_t seq = mouse_seq();
        if (seq != last_seq) {
            last_seq = seq;
            mx = mouse_x();
            my = mouse_y();
            int buttons = mouse_buttons();
            int left = buttons & MOUSE_LEFT;
            int was_left = prev_buttons & MOUSE_LEFT;

            if (left && !was_left) {
                switch (handle_press(mx, my)) {
                    case ACT_EXIT:     goto done;
                    case ACT_REBOOT:   do_reboot();     break;   /* no return */
                    case ACT_POWEROFF: do_poweroff();   break;   /* no return */
                    case ACT_SCENE:    scene_full = 1;  break;
                    default: break;
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
                    w->x = nx; w->y = ny;
                    int lo_x = (ox < nx ? ox : nx) - 1;
                    int lo_y = (oy < ny ? oy : ny) - 1;
                    int hi_x = (ox > nx ? ox : nx) + w->w + 6;
                    int hi_y = (oy > ny ? oy : ny) + w->h + 6;
                    dirty = 1;
                    dx0 = lo_x; dy0 = lo_y; dw = hi_x - lo_x; dh = hi_y - lo_y;
                }
            }

            /* live Start-menu hover highlight */
            if (start_open) {
                int hv = menu_item_at(mx, my);
                if (hv != start_hover) { start_hover = hv; scene_full = 1; }
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
            render_scene();
            gfx_present();
            draw_cursor_fb(mx, my);
            cx = mx; cy = my;
        } else {
            if (dirty) {
                render_scene();
                gfx_present_rect(dx0, dy0, dw, dh);
            } else if (scene_taskbar) {
                render_scene();
                gfx_present_rect(0, H - TB_H, W, TB_H);
                if (cy + ch > H - TB_H)
                    draw_cursor_fb(cx, cy);
            }
            if (dirty || mx != cx || my != cy) {
                gfx_present_rect(cx, cy, cw, ch);      /* restore under old cursor */
                draw_cursor_fb(mx, my);
                cx = mx; cy = my;
            }
        }

        __asm__ volatile("sti; hlt");
    }

done:
    leave_graphics();
}
