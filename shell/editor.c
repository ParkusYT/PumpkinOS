/* ===========================================================================
 * PumpkinOS - a small full-screen text editor
 * ---------------------------------------------------------------------------
 * Loads a file into a flat buffer, draws it straight to VGA text memory with a
 * title bar and a status/help line, and lets you move around with the arrow
 * keys, type, backspace/delete and insert newlines. Esc quits (offering to
 * save if the buffer changed). Lines longer than the screen are truncated in
 * the display; the file keeps the full content.
 * ========================================================================= */
#include "editor.h"
#include "keyboard.h"
#include "console.h"
#include "fat12.h"
#include "string.h"
#include "io.h"
#include <stdint.h>

#define COLS       80
#define ROWS       25
#define TEXT_TOP   1                 /* first text row (row 0 = title)        */
#define TEXT_ROWS  (ROWS - 2)        /* rows 1..23; row 24 = status           */
#define EDIT_MAX   (64 * 1024)

#define ATTR_TEXT   0x07             /* light grey on black */
#define ATTR_TITLE  0x1F             /* white on blue       */
#define ATTR_STATUS 0x70             /* black on light grey */

static volatile uint16_t *const VRAM = (uint16_t *)0xB8000;

static char buf[EDIT_MAX];
static int  len;                     /* bytes in buf                          */
static int  cursor;                  /* insertion point, 0..len               */
static int  top_line;                /* first visible line (0-indexed)        */
static int  goal_col;                /* preferred column for up/down movement */
static int  modified;
static char fname[13];

/* ---- raw screen ----------------------------------------------------------- */
static void put_cell(int row, int col, char ch, uint8_t attr) {
    VRAM[row * COLS + col] = (uint16_t)(uint8_t)ch | ((uint16_t)attr << 8);
}

static void put_str(int row, int col, const char *s, uint8_t attr) {
    for (; *s && col < COLS; s++, col++)
        put_cell(row, col, *s, attr);
}

static void fill_row(int row, char ch, uint8_t attr) {
    for (int c = 0; c < COLS; c++)
        put_cell(row, c, ch, attr);
}

static void set_hw_cursor(int row, int col) {
    uint16_t pos = (uint16_t)(row * COLS + col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)pos);
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

/* ---- buffer geometry ------------------------------------------------------ */
static int total_lines(void) {
    int n = 1;
    for (int i = 0; i < len; i++)
        if (buf[i] == '\n') n++;
    return n;
}

static int line_start(int line) {
    if (line <= 0) return 0;
    int l = 0;
    for (int i = 0; i < len; i++)
        if (buf[i] == '\n' && ++l == line)
            return i + 1;
    return len;
}

static int line_len(int line) {
    int s = line_start(line), e = s;
    while (e < len && buf[e] != '\n') e++;
    return e - s;
}

static void cursor_rc(int *line, int *col) {
    int l = 0, ls = 0;
    for (int i = 0; i < cursor; i++)
        if (buf[i] == '\n') { l++; ls = i + 1; }
    *line = l;
    *col  = cursor - ls;
}

/* ---- rendering ------------------------------------------------------------ */
static void draw_status(const char *msg) {
    fill_row(ROWS - 1, ' ', ATTR_STATUS);
    put_str(ROWS - 1, 1, msg, ATTR_STATUS);
}

static void render(void) {
    int cl, cc;
    cursor_rc(&cl, &cc);

    /* keep the cursor line on screen */
    if (cl < top_line) top_line = cl;
    if (cl >= top_line + TEXT_ROWS) top_line = cl - TEXT_ROWS + 1;

    /* title bar */
    fill_row(0, ' ', ATTR_TITLE);
    put_str(0, 1, "PumpkinOS edit  -  ", ATTR_TITLE);
    put_str(0, 20, fname, ATTR_TITLE);
    if (modified) put_str(0, 20 + (int)strlen(fname) + 1, "[modified]", ATTR_TITLE);

    /* text */
    int idx = line_start(top_line);
    for (int r = 0; r < TEXT_ROWS; r++) {
        int col = 0;
        while (idx < len && buf[idx] != '\n' && col < COLS)
            put_cell(TEXT_TOP + r, col++, buf[idx++], ATTR_TEXT);
        for (int c = col; c < COLS; c++)          /* clear rest of row */
            put_cell(TEXT_TOP + r, c, ' ', ATTR_TEXT);
        while (idx < len && buf[idx] != '\n') idx++;   /* skip overflow */
        if (idx < len && buf[idx] == '\n') idx++;
    }

    /* status/help + hardware cursor */
    char pos[40], *p = pos;
    const char *lbl = "  Ln ";
    while (*lbl) *p++ = *lbl++;
    /* line/col numbers (1-based) */
    int ln = cl + 1, co = cc + 1;
    char tmp[12]; int t = 0;
    do { tmp[t++] = (char)('0' + ln % 10); ln /= 10; } while (ln);
    while (t) *p++ = tmp[--t];
    *p++ = ','; *p++ = ' ';
    do { tmp[t++] = (char)('0' + co % 10); co /= 10; } while (co);
    while (t) *p++ = tmp[--t];
    *p = '\0';

    draw_status("Arrows move   Enter/Bksp/Del edit   Esc = save & quit");
    put_str(ROWS - 1, 56, pos, ATTR_STATUS);

    int scr_col = cc < COLS ? cc : COLS - 1;
    set_hw_cursor(TEXT_TOP + (cl - top_line), scr_col);
}

/* ---- editing --------------------------------------------------------------- */
static void insert_ch(char c) {
    if (len >= EDIT_MAX - 1) return;
    for (int i = len; i > cursor; i--) buf[i] = buf[i - 1];
    buf[cursor++] = c;
    len++;
    modified = 1;
}

static void backspace(void) {
    if (cursor <= 0) return;
    for (int i = cursor - 1; i < len - 1; i++) buf[i] = buf[i + 1];
    cursor--; len--;
    modified = 1;
}

static void delete_ch(void) {
    if (cursor >= len) return;
    for (int i = cursor; i < len - 1; i++) buf[i] = buf[i + 1];
    len--;
    modified = 1;
}

/* move to (line, min(goal_col, line length)) */
static void goto_line(int line) {
    int nlines = total_lines();
    if (line < 0) line = 0;
    if (line > nlines - 1) line = nlines - 1;
    int ll = line_len(line);
    int col = goal_col < ll ? goal_col : ll;
    cursor = line_start(line) + col;
}

/* ---- main ----------------------------------------------------------------- */
static void save(void) {
    if (fs_create(fname, buf, (uint32_t)len) == 0)
        modified = 0;
}

void editor_run(const char *name) {
    /* filename (8.3, upper-cased like the rest of the FS) */
    int i = 0;
    for (; name[i] && i < 12; i++) {
        char c = name[i];
        fname[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    fname[i] = '\0';

    int n = fs_read(fname, (uint8_t *)buf, EDIT_MAX);
    len = n > 0 ? n : 0;
    cursor = 0; top_line = 0; goal_col = 0; modified = 0;

    for (;;) {
        render();
        unsigned char c = (unsigned char)keyboard_getchar();
        int cl, cc;

        switch (c) {
        case 27:                                   /* Esc: quit */
            if (!modified) { console_clear(); return; }
            draw_status("Save changes?   Y = save & quit    N = discard    Esc = cancel");
            {
                unsigned char k = (unsigned char)keyboard_getchar();
                if (k == 'y' || k == 'Y') { save(); console_clear(); return; }
                if (k == 'n' || k == 'N') { console_clear(); return; }
            }
            break;                                 /* cancel -> keep editing */

        case KEY_LEFT:
            if (cursor > 0) cursor--;
            cursor_rc(&cl, &cc); goal_col = cc;
            break;
        case KEY_RIGHT:
            if (cursor < len) cursor++;
            cursor_rc(&cl, &cc); goal_col = cc;
            break;
        case KEY_UP:
            cursor_rc(&cl, &cc); goto_line(cl - 1);
            break;
        case KEY_DOWN:
            cursor_rc(&cl, &cc); goto_line(cl + 1);
            break;
        case KEY_HOME:
            cursor_rc(&cl, &cc); cursor = line_start(cl); goal_col = 0;
            break;
        case KEY_END:
            cursor_rc(&cl, &cc);
            cursor = line_start(cl) + line_len(cl);
            cursor_rc(&cl, &cc); goal_col = cc;
            break;
        case KEY_PGUP:
            cursor_rc(&cl, &cc); goto_line(cl - TEXT_ROWS);
            break;
        case KEY_PGDN:
            cursor_rc(&cl, &cc); goto_line(cl + TEXT_ROWS);
            break;
        case KEY_DEL:
            delete_ch();
            break;

        case '\n':
            insert_ch('\n');
            cursor_rc(&cl, &cc); goal_col = cc;
            break;
        case '\b':
            backspace();
            cursor_rc(&cl, &cc); goal_col = cc;
            break;
        case '\t':
            for (int s = 0; s < 4; s++) insert_ch(' ');
            cursor_rc(&cl, &cc); goal_col = cc;
            break;

        default:
            if (c >= ' ' && c < 127) {
                insert_ch((char)c);
                cursor_rc(&cl, &cc); goal_col = cc;
            }
            break;
        }
    }
}
