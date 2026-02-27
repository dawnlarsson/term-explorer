#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <math.h>

#ifdef __linux__
#include <linux/input.h>
#endif

#define KEY_UP 1000
#define KEY_DOWN 1001
#define KEY_RIGHT 1002
#define KEY_LEFT 1003
#define KEY_PAGE_UP 1004
#define KEY_PAGE_DOWN 1005
#define KEY_END 1006
#define KEY_HOME 1007
#define KEY_SHIFT_PAGE_UP 1008
#define KEY_SHIFT_PAGE_DOWN 1009
#define KEY_ENTER 10
#define KEY_ESC 27
#define KEY_BACKSPACE 127

typedef struct
{
        int r, g, b;
} Color;

typedef struct
{
        char ch[5];
        Color fg, bg;
        bool bold, invert;
} Cell;

typedef struct
{
        int x, y, sub_y, wheel;
        bool has_sub, left, right, clicked, right_clicked, hide_cursor;
} Mouse;

Mouse term_mouse;
int term_width, term_height;

static struct termios orig_termios;
static volatile int resize_flag = 1;
static Cell *canvas;
static int fd_m = -1, raw_mx, raw_my, color_mode = 0;
static bool is_evdev = false;
static char out_buf[1024 * 1024];

static void on_resize(int s) { resize_flag = 1; }
static void on_sigint(int s)
{
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\033%%@\033[0m\033[2J\033[H\033[?25h\033[?7h\033[?1006l\033[?1015l\033[?1003l");
        fflush(stdout);
        _exit(1);
}

static bool col_eq(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static int rgb256(Color c) { return 16 + (36 * (c.r * 5 / 255)) + (6 * (c.g * 5 / 255)) + (c.b * 5 / 255); }
static int rgb_to_ansi16(Color c, bool is_bg)
{
        int r = c.r > 127, g = c.g > 127, b = c.b > 127, bright = (c.r > 191 || c.g > 191 || c.b > 191);
        if (!r && !g && !b && (c.r || c.g || c.b))
                bright = 1;
        return (is_bg ? (bright ? 100 : 40) : (bright ? 90 : 30)) + (r | (g << 1) | (b << 2));
}

#define SET_COL(fg, c) (color_mode == 2 ? printf("\033[%d;2;%d;%d;%dm", fg ? 38 : 48, c.r, c.g, c.b) : color_mode == 1 ? printf("\033[%d;5;%dm", fg ? 38 : 48, rgb256(c)) \
                                                                                                                       : printf("\033[%dm", rgb_to_ansi16(c, !fg)))

void term_restore(void)
{
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\033%%@\033[0m\033[2J\033[H\033[?25h\033[?7h\033[?1006l\033[?1015l\033[?1003l");
        fflush(stdout);
        if (fd_m >= 0)
                close(fd_m);
        if (canvas)
                free(canvas);
}

int term_init(void)
{
        (tcgetattr(STDIN_FILENO, &orig_termios) == 0) orelse return 0;
        struct termios t_raw = orig_termios;
        t_raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        t_raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        t_raw.c_oflag &= ~(OPOST);
        t_raw.c_cflag |= CS8;
        t_raw.c_cc[VMIN] = 0;
        t_raw.c_cc[VTIME] = 0;
        (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t_raw) == 0) orelse return 0;

        signal(SIGINT, on_sigint);
        signal(SIGTERM, on_sigint);
        signal(SIGQUIT, on_sigint);
        char *ct = getenv("COLORTERM"), *term = getenv("TERM");
        color_mode = (ct && (!strcmp(ct, "truecolor") || !strcmp(ct, "24bit"))) ? 2 : (term && strstr(term, "256color")) ? 1
                                                                                                                         : 0;

#ifdef __linux__
        for (int i = 0; i < 32 && fd_m < 0; i++)
        {
                char path[64];
                snprintf(path, sizeof(path), "/dev/input/event%d", i);
                int fd = open(path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0)
                {
                        unsigned long ev[EV_MAX / 8 + 1], rel[REL_MAX / 8 + 1];
                        ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
                        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
                        if ((ev[EV_REL / 8] & (1 << (EV_REL % 8))) && (rel[REL_X / 8] & (1 << (REL_X % 8))) && (rel[REL_Y / 8] & (1 << (REL_Y % 8))))
                        {
                                fd_m = fd;
                                is_evdev = true;
                        }
                        else
                                close(fd);
                }
        }
#endif
        if (fd_m < 0)
                fd_m = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
        setvbuf(stdout, out_buf, _IOFBF, sizeof(out_buf));
        printf("\033%%G\033[?25l\033[?7l\033[?1003h\033[?1015h\033[?1006h");
        signal(SIGWINCH, on_resize);
        return 1;
}

int term_poll(int timeout_ms)
{
        struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {fd_m, POLLIN, 0}};
        poll(fds, fd_m >= 0 ? 2 : 1, timeout_ms);

        if (resize_flag)
        {
                struct winsize ws;
                ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
                term_width = ws.ws_col;
                term_height = ws.ws_row;
                resize_flag = 0;
        }

        static bool init_mouse = true;
        if (init_mouse && term_width > 0 && term_height > 0)
        {
                raw_mx = (term_width / 2) * 8;
                raw_my = (term_height / 2) * 16;
                term_mouse.x = term_width / 2;
                term_mouse.y = term_height / 2;
                init_mouse = false;
        }

        bool last_left = term_mouse.left;
        bool last_right = term_mouse.right;
        term_mouse.wheel = 0;
        int key = 0;

        if (fd_m >= 0)
        {
#ifdef __linux__
                if (is_evdev)
                {
                        struct input_event ev;
                        while (read(fd_m, &ev, sizeof(ev)) == sizeof(ev))
                        {
                                term_mouse.hide_cursor = false;
                                if (ev.type == EV_REL && ev.code == REL_X)
                                        raw_mx += ev.value;
                                if (ev.type == EV_REL && ev.code == REL_Y)
                                        raw_my += ev.value;
                                if (ev.type == EV_REL && ev.code == REL_WHEEL)
                                        term_mouse.wheel -= ev.value;
                                if (ev.type == EV_KEY && ev.code == BTN_LEFT)
                                        term_mouse.left = ev.value;
                                if (ev.type == EV_KEY && ev.code == BTN_RIGHT)
                                        term_mouse.right = ev.value;
                        }
                }
                else
#endif
                {
                        unsigned char m[3];
                        while (read(fd_m, m, 3) == 3)
                        {
                                term_mouse.hide_cursor = false;
                                term_mouse.left = m[0] & 1;
                                term_mouse.right = m[0] & 2;
                                raw_mx += (signed char)m[1];
                                raw_my -= (signed char)m[2];
                        }
                }
                raw_mx = raw_mx < 0 ? 0 : raw_mx >= term_width * 8 ? term_width * 8 - 1
                                                                   : raw_mx;
                raw_my = raw_my < 0 ? 0 : raw_my >= term_height * 16 ? term_height * 16 - 1
                                                                     : raw_my;
                term_mouse.x = raw_mx / 8;
                term_mouse.y = raw_my / 16;
                term_mouse.sub_y = (raw_my / 8) % 2;
                term_mouse.has_sub = true;
        }

        raw char buf[64];
        int n = read(STDIN_FILENO, buf, sizeof(buf));
        for (int i = 0; i < n; i++)
        {
                if (buf[i] == '\033')
                {
                        if (i + 3 < n && buf[i + 3] == '~')
                        {
                                if (buf[i + 2] == '5')
                                        key = KEY_PAGE_UP;
                                else if (buf[i + 2] == '6')
                                        key = KEY_PAGE_DOWN;

                                if (key)
                                {
                                        i += 3;
                                        continue;
                                }
                        }
                        if (i + 2 < n && buf[i + 2] == '<')
                        {
                                int b, x, y, offset;
                                char m;
                                if (sscanf(buf + i + 3, "%d;%d;%d%c%n", &b, &x, &y, &m, &offset) == 4)
                                {
                                        term_mouse.hide_cursor = true;
                                        term_mouse.x = x - 1;
                                        term_mouse.y = y - 1;
                                        term_mouse.has_sub = false;
                                        bool d = (m == 'M');
                                        if (b == 0 || b == 32)
                                                term_mouse.left = d;
                                        if (b == 2 || b == 34)
                                                term_mouse.right = d;
                                        if (b == 64 && d)
                                                term_mouse.wheel--;
                                        if (b == 65 && d)
                                                term_mouse.wheel++;
                                        i += 3 + offset;
                                        continue;
                                }
                        }
                        else if (i + 5 < n && buf[i + 5] == '~' && buf[i + 3] == ';' && buf[i + 4] == '2')
                        {
                                if (buf[i + 2] == '5')
                                        key = KEY_SHIFT_PAGE_UP;
                                else if (buf[i + 2] == '6')
                                        key = KEY_SHIFT_PAGE_DOWN;
                                if (key)
                                {
                                        i += 5;
                                        continue;
                                }
                        }
                        else if (i + 3 < n && buf[i + 3] == '~')
                        {
                                if (buf[i + 2] == '5')
                                        key = KEY_PAGE_UP;
                                else if (buf[i + 2] == '6')
                                        key = KEY_PAGE_DOWN;
                                else if (buf[i + 2] == '4' || buf[i + 2] == '8')
                                        key = KEY_END;
                                else if (buf[i + 2] == '1' || buf[i + 2] == '7')
                                        key = KEY_HOME;
                                if (key)
                                {
                                        i += 3;
                                        continue;
                                }
                        }
                        else if (i + 2 < n && buf[i + 1] == 'O')
                        {
                                if (buf[i + 2] == 'F')
                                        key = KEY_END;
                                else if (buf[i + 2] == 'H')
                                        key = KEY_HOME;
                                if (key)
                                {
                                        i += 2;
                                        continue;
                                }
                        }
                        else if (i + 2 < n)
                        {
                                if (buf[i + 2] == 'A')
                                        key = KEY_UP;
                                else if (buf[i + 2] == 'B')
                                        key = KEY_DOWN;
                                else if (buf[i + 2] == 'C')
                                        key = KEY_RIGHT;
                                else if (buf[i + 2] == 'D')
                                        key = KEY_LEFT;
                                else if (buf[i + 2] == 'F')
                                        key = KEY_END;
                                else if (buf[i + 2] == 'H')
                                        key = KEY_HOME;
                                if (key)
                                {
                                        i += 2;
                                        continue;
                                }
                        }
                        else if (i + 2 < n)
                        {
                                if (buf[i + 2] == 'A')
                                        key = KEY_UP;
                                else if (buf[i + 2] == 'B')
                                        key = KEY_DOWN;
                                else if (buf[i + 2] == 'C')
                                        key = KEY_RIGHT;
                                else if (buf[i + 2] == 'D')
                                        key = KEY_LEFT;
                                if (key)
                                {
                                        i += 2;
                                        continue;
                                }
                        }
                }
                if (!key)
                        key = (buf[i] == '\r') ? KEY_ENTER : (buf[i] == 127 || buf[i] == 8) ? KEY_BACKSPACE
                                                                                            : buf[i];
        }
        term_mouse.clicked = (!last_left && term_mouse.left);
        term_mouse.right_clicked = (!last_right && term_mouse.right);
        return key;
}

void ui_begin(void)
{
        static int cw, ch;
        if (term_width != cw || term_height != ch)
        {
                canvas = realloc(canvas, term_width * term_height * sizeof(Cell)) orelse { exit(1); };
                cw = term_width;
                ch = term_height;
        }
}

void ui_clear(Color bg)
{
        for (int i = 0; i < term_width * term_height; i++)
                canvas[i] = (Cell){" ", {255, 255, 255}, bg, false, false};
}

void ui_rect(int x, int y, int w, int h, Color bg)
{
        for (int r = y; r < y + h; r++)
        {
                if (r < 0 || r >= term_height)
                        continue;
                for (int c = x; c < x + w; c++)
                {
                        if (c < 0 || c >= term_width)
                                continue;
                        canvas[r * term_width + c].bg = bg;
                        canvas[r * term_width + c].fg = bg;
                        canvas[r * term_width + c].invert = false;
                        canvas[r * term_width + c].bold = false;
                        strcpy(canvas[r * term_width + c].ch, " ");
                }
        }
}

void ui_text(int x, int y, const char *txt, Color fg, Color bg, bool bold, bool invert)
{
        if (y < 0 || y >= term_height)
                return;
        for (int i = 0, sx = x; txt[i] && sx < term_width; sx++)
        {
                int len = ((txt[i] & 0xE0) == 0xC0) ? 2 : ((txt[i] & 0xF0) == 0xE0) ? 3
                                                      : ((txt[i] & 0xF8) == 0xF0)   ? 4
                                                                                    : 1;
                int actual_len = 0;

                if (sx >= 0)
                {
                        Cell *c = &canvas[y * term_width + sx];
                        for (int j = 0; j < len && txt[i + j]; j++)
                        {
                                c->ch[j] = txt[i + j];
                                actual_len++;
                        }
                        c->ch[actual_len] = '\0';
                        c->fg = fg;
                        c->bg = bg;
                        c->bold = bold;
                        c->invert = invert;
                }
                else
                {
                        for (int j = 0; j < len && txt[i + j]; j++)
                                actual_len++;
                }

                i += actual_len > 0 ? actual_len : 1;
        }
}

void ui_text_centered(int x, int y, int w, const char *txt, Color fg, Color bg, bool bold, bool invert)
{
        int len = 0;
        for (int i = 0; txt[i]; i++)
                if ((txt[i] & 0xC0) != 0x80)
                        len++;
        int px = x + (w - len) / 2;
        ui_text(px < x ? x : px, y, txt, fg, bg, bold, invert);
}

static void ui_cursor(void)
{
        if (term_mouse.hide_cursor)
                return;

        if (term_mouse.x >= 0 && term_mouse.x < term_width && term_mouse.y >= 0 && term_mouse.y < term_height)
        {
                int idx = term_mouse.y * term_width + term_mouse.x;
                strcpy(canvas[idx].ch, term_mouse.has_sub ? (term_mouse.sub_y == 0 ? "\xe2\x96\x80" : "\xe2\x96\x84") : "\xe2\x96\xa0");
                canvas[idx].fg = term_mouse.left ? (Color){0, 255, 0} : (term_mouse.right ? (Color){255, 0, 0} : (Color){255, 255, 0});
        }
}

void ui_end(void)
{
        ui_cursor();

        Color lfg = {-1, -1, -1}, lbg = {-1, -1, -1};
        bool lbold = false, linvert = false;
        printf("\033[H");

        for (int i = 0; i < term_height * term_width; i++)
        {
                if (i > 0 && i % term_width == 0)
                        printf("\r\n");
                Cell c = canvas[i];
                if (c.bold != lbold)
                {
                        printf(c.bold ? "\033[1m" : "\033[22m");
                        lbold = c.bold;
                }
                if (c.invert != linvert)
                {
                        printf(c.invert ? "\033[7m" : "\033[27m");
                        linvert = c.invert;
                }
                if (!col_eq(c.bg, lbg))
                {
                        SET_COL(0, c.bg);
                        lbg = c.bg;
                }
                if (!col_eq(c.fg, lfg))
                {
                        SET_COL(1, c.fg);
                        lfg = c.fg;
                }
                fputs(c.ch, stdout);
        }
        printf("\x1b[0m");
        fflush(stdout);
}

typedef enum
{
        UI_MODE_GRID,
        UI_MODE_LIST
} UIListMode;

typedef struct
{
        int x, y, w, h;
        int item_count;
        int cell_w, cell_h;
        Color bg, scrollbar_bg, scrollbar_fg;
} UIListParams;

typedef struct
{
        float target_scroll, current_scroll, scroll_velocity;
        float drag_offset;
        bool dragging_scroll;
        int selected_idx;
        UIListMode mode;
        bool clicked_on_item;
        UIListParams p;

        int last_nav_key;
        int nav_key_streak;
        long long last_nav_time;
} UIListState;

typedef struct
{
        bool active;
        int x, y;
        int w, h;
        UIListState list;
} UIContextState;

typedef struct
{
        int x, y, w, h;
        bool hovered;
        bool pressed;
        bool clicked;
        bool right_clicked;
} UIItemResult;

static UIContextState global_ctx;
static int global_ctx_target = -1;

void ui_list_reset(UIListState *s)
{
        s->target_scroll = 0;
        s->current_scroll = 0;
        s->scroll_velocity = 0;
        s->drag_offset = 0;
        s->dragging_scroll = false;
        s->selected_idx = -1;
        s->clicked_on_item = false;

        s->last_nav_key = 0;
        s->nav_key_streak = 0;
        s->last_nav_time = 0;
}

void ui_list_set_mode(UIListState *s, const UIListParams *p, UIListMode mode)
{
        if (s->mode == mode)
                return;

        s->mode = mode;

        if (s->selected_idx >= 0)
        {
                int cols = (s->mode == UI_MODE_LIST) ? 1 : ((p->w - 1) / p->cell_w > 0 ? (p->w - 1) / p->cell_w : 1);
                int c_h = (s->mode == UI_MODE_LIST) ? 1 : p->cell_h;
                int row = s->selected_idx / cols;

                s->target_scroll = (float)(row * c_h);
                s->current_scroll = s->target_scroll;
        }
}

void ui_list_begin(UIListState *s, const UIListParams *p, int key)
{
        s->p = *p;
        s->clicked_on_item = false;

        int cols = (s->mode == UI_MODE_LIST) ? 1 : ((s->p.w - 1) / s->p.cell_w > 0 ? (s->p.w - 1) / s->p.cell_w : 1);
        int c_h = (s->mode == UI_MODE_LIST) ? 1 : s->p.cell_h;
        int rows = (s->p.item_count + cols - 1) / cols;
        int rows_per_page = s->p.h / c_h > 0 ? s->p.h / c_h : 1;
        int items_per_page = rows_per_page * cols;

        struct timeval tv;
        gettimeofday(&tv, NULL);
        long long now_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;

        if (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT)
        {
                if (key == s->last_nav_key && (now_ms - s->last_nav_time) < 400)
                {
                        s->nav_key_streak++;
                }
                else
                {
                        s->last_nav_key = key;
                        s->nav_key_streak = 1;
                }
                s->last_nav_time = now_ms;
        }
        else if (key != 0)
        {
                s->last_nav_key = 0;
                s->nav_key_streak = 0;
        }

        int step = 1;
        if (s->nav_key_streak > 30)
                step = (s->mode == UI_MODE_LIST) ? 25 : 8;
        else if (s->nav_key_streak > 15)
                step = (s->mode == UI_MODE_LIST) ? 10 : 4;
        else if (s->nav_key_streak > 5)
                step = (s->mode == UI_MODE_LIST) ? 3 : 2;

        int old_idx = s->selected_idx;

        if (s->selected_idx == -1 && s->p.item_count > 0 &&
            (key >= KEY_UP && key <= KEY_SHIFT_PAGE_DOWN))
        {
                s->selected_idx = 0;
        }
        else if (s->selected_idx >= 0)
        {
                if (key == KEY_RIGHT)
                {
                        s->selected_idx += step;
                        if (s->selected_idx >= s->p.item_count)
                                s->selected_idx = s->p.item_count - 1;
                }
                if (key == KEY_LEFT)
                {
                        s->selected_idx -= step;
                        if (s->selected_idx < 0)
                                s->selected_idx = 0;
                }
                if (key == KEY_DOWN)
                {
                        s->selected_idx += cols * step;
                        if (s->selected_idx >= s->p.item_count)
                                s->selected_idx = s->p.item_count - 1;
                }
                if (key == KEY_UP)
                {
                        s->selected_idx -= cols * step;
                        if (s->selected_idx < 0)
                                s->selected_idx = old_idx % cols;
                }
                if (key == KEY_PAGE_DOWN)
                {
                        s->selected_idx += items_per_page;
                        if (s->selected_idx >= s->p.item_count)
                                s->selected_idx = s->p.item_count - 1;
                }
                if (key == KEY_PAGE_UP)
                {
                        s->selected_idx -= items_per_page;
                        if (s->selected_idx < 0)
                                s->selected_idx = old_idx % cols;
                }
                if (key == KEY_END)
                {
                        if (s->selected_idx == s->p.item_count - 1)
                                s->selected_idx = 0;
                        else
                                s->selected_idx = s->p.item_count - 1;
                }
                if (key == KEY_HOME || key == KEY_SHIFT_PAGE_UP)
                {
                        s->selected_idx = 0;
                }
                if (key == KEY_SHIFT_PAGE_DOWN)
                {
                        s->selected_idx = s->p.item_count - 1;
                }
        }

        if (old_idx != s->selected_idx && s->selected_idx >= 0)
        {
                int row = s->selected_idx / cols;
                int item_top = row * c_h;
                if (item_top < (int)s->target_scroll)
                        s->target_scroll = (float)item_top;
                else if (item_top + c_h > (int)s->target_scroll + s->p.h)
                        s->target_scroll = (float)(item_top + c_h - s->p.h);
        }

        int max_scroll = rows * c_h > s->p.h ? rows * c_h - s->p.h : 0;
        int thumb_h = s->p.h * s->p.h / (rows * c_h > s->p.h ? rows * c_h : s->p.h);
        if (thumb_h < 1)
                thumb_h = 1;

        if (term_mouse.clicked && term_mouse.x == s->p.x + s->p.w - 1 && term_mouse.y >= s->p.y && term_mouse.y < s->p.y + s->p.h)
        {
                s->dragging_scroll = true;
                int thumb_top = s->p.y + (max_scroll > 0 ? (int)((s->current_scroll / max_scroll) * (s->p.h - thumb_h)) : 0);
                if (term_mouse.y >= thumb_top && term_mouse.y < thumb_top + thumb_h)
                        s->drag_offset = (float)(term_mouse.y - thumb_top);
                else
                        s->drag_offset = (float)thumb_h / 2.0f;
        }

        if (!term_mouse.left)
                s->dragging_scroll = false;

        if (s->dragging_scroll && max_scroll > 0)
        {
                float cr = (float)(term_mouse.y - s->p.y - s->drag_offset) / (s->p.h - thumb_h > 0 ? s->p.h - thumb_h : 1);
                s->target_scroll = s->current_scroll = (cr < 0 ? 0 : cr > 1 ? 1
                                                                            : cr) *
                                                       max_scroll;
                s->scroll_velocity = 0.0f;
        }

        float friction = 0.82f;
        float wheel_val = term_mouse.wheel;

        if (wheel_val != 0)
        {
                float base_power = (s->mode == UI_MODE_LIST) ? 0.18f : ((float)c_h * 0.1f);

                float wheel_dir = wheel_val > 0 ? 1.0f : -1.0f;
                float vel_dir = s->scroll_velocity > 0 ? 1.0f : (s->scroll_velocity < 0 ? -1.0f : 0.0f);

                if (vel_dir == wheel_dir && fabsf(s->scroll_velocity) > 0.05f)
                        base_power *= (2.5f + fabsf(s->scroll_velocity) * 1.2f);

                s->scroll_velocity += wheel_val * base_power;
        }

        s->target_scroll += s->scroll_velocity;
        s->scroll_velocity *= friction;

        if (s != &global_ctx.list && (s->dragging_scroll || wheel_val != 0))
        {
                global_ctx.active = false;
                global_ctx.w = 0;
        }

        if (s->scroll_velocity > -0.05f && s->scroll_velocity < 0.05f)
                s->scroll_velocity = 0.0f;

        if (s->target_scroll > max_scroll)
        {
                s->target_scroll = max_scroll;
                s->scroll_velocity = 0.0f;
        }
        else if (s->target_scroll < 0)
        {
                s->target_scroll = 0;
                s->scroll_velocity = 0.0f;
        }

        if (!s->dragging_scroll && s->scroll_velocity == 0.0f)
        {
                float snap_target = (float)(((int)(s->target_scroll + (c_h / 2.0f)) / c_h) * c_h);
                if (snap_target <= max_scroll)
                        s->target_scroll = snap_target;
        }

        if (!s->dragging_scroll)
        {
                s->current_scroll += (s->target_scroll - s->current_scroll) * 0.3f;
                if (s->target_scroll - s->current_scroll > -0.05f && s->target_scroll - s->current_scroll < 0.05f)
                        s->current_scroll = s->target_scroll;
        }
}

bool ui_list_do_item(UIListState *s, int index, UIItemResult *res)
{
        int cols = (s->mode == UI_MODE_LIST) ? 1 : ((s->p.w - 1) / s->p.cell_w > 0 ? (s->p.w - 1) / s->p.cell_w : 1);
        int c_w = (s->mode == UI_MODE_LIST) ? s->p.w - 1 : s->p.cell_w;
        int c_h = (s->mode == UI_MODE_LIST) ? 1 : s->p.cell_h;

        int screen_x = s->p.x + (index % cols) * c_w;
        int screen_y = s->p.y + (index / cols * c_h) - (int)(s->current_scroll + 0.5f);

        if (screen_y + c_h <= s->p.y || screen_y >= s->p.y + s->p.h)
                return false;

        res->x = screen_x;
        res->y = screen_y;
        res->w = c_w;
        res->h = c_h;

        bool is_ctx_list = (s == &global_ctx.list);

        bool over_ctx_menu = global_ctx.active && !is_ctx_list &&
                             term_mouse.x >= global_ctx.x && term_mouse.x < global_ctx.x + global_ctx.w &&
                             term_mouse.y >= global_ctx.y && term_mouse.y < global_ctx.y + global_ctx.h;

        res->hovered = (!over_ctx_menu && !s->dragging_scroll &&
                        term_mouse.x >= screen_x && term_mouse.x < screen_x + c_w &&
                        term_mouse.y >= screen_y && term_mouse.y < screen_y + c_h &&
                        term_mouse.y >= s->p.y && term_mouse.y < s->p.y + s->p.h && term_mouse.x < s->p.x + s->p.w - 1);

        res->pressed = (res->hovered && term_mouse.left);
        res->clicked = (res->hovered && term_mouse.clicked);
        res->right_clicked = (res->hovered && term_mouse.right_clicked);

        if (res->clicked || res->right_clicked)
        {
                s->selected_idx = index;
                s->clicked_on_item = true;
        }
        return true;
}

void ui_list_end(UIListState *s)
{
        int cols = (s->mode == UI_MODE_LIST) ? 1 : ((s->p.w - 1) / s->p.cell_w > 0 ? (s->p.w - 1) / s->p.cell_w : 1);
        int c_h = (s->mode == UI_MODE_LIST) ? 1 : s->p.cell_h;
        int rows = (s->p.item_count + cols - 1) / cols;
        int max_scroll = rows * c_h > s->p.h ? rows * c_h - s->p.h : 0;

        if (term_mouse.clicked && !s->clicked_on_item && !s->dragging_scroll && !global_ctx.active)
        {
                if (term_mouse.x >= s->p.x && term_mouse.x < s->p.x + s->p.w - 1 &&
                    term_mouse.y >= s->p.y && term_mouse.y < s->p.y + s->p.h)
                {
                        s->selected_idx = -1;
                }
        }

        if (max_scroll > 0)
        {
                int thumb_h = s->p.h * s->p.h / (rows * c_h);
                if (thumb_h < 1)
                        thumb_h = 1;
                ui_rect(s->p.x + s->p.w - 1, s->p.y, 1, s->p.h, s->p.scrollbar_bg);

                Color thumb_col = s->dragging_scroll ? (Color){255, 255, 255} : s->p.scrollbar_fg;
                ui_rect(s->p.x + s->p.w - 1, s->p.y + (int)((s->current_scroll / max_scroll) * (s->p.h - thumb_h)), 1, thumb_h, thumb_col);
        }
}

bool ui_list_is_animating(UIListState *s)
{
        if (s->dragging_scroll)
                return true;
        float diff = s->target_scroll - s->current_scroll;
        return (diff > 0.01f || diff < -0.01f || s->scroll_velocity > 0.01f || s->scroll_velocity < -0.01f);
}

void ui_context_open(int target_idx)
{
        global_ctx.active = true;
        global_ctx.x = term_mouse.x;
        global_ctx.y = term_mouse.y;
        global_ctx.w = 0;
        global_ctx_target = target_idx;

        global_ctx.list.current_scroll = 0;
        global_ctx.list.target_scroll = 0;
        global_ctx.list.scroll_velocity = 0;
        global_ctx.list.dragging_scroll = false;
        global_ctx.list.selected_idx = -1;
        global_ctx.list.mode = UI_MODE_LIST;

        term_mouse.right_clicked = false;
}

int ui_context_target(void)
{
        return global_ctx_target;
}

void ui_context_close(void)
{
        global_ctx.active = false;
        global_ctx.w = 0;
}

bool ui_context_do(const char **items, int count, int *out_idx)
{
        if (!global_ctx.active)
                return false;

        if (global_ctx.w == 0)
        {
                int max_w = 0;
                for (int i = 0; i < count; i++)
                {
                        int len = strlen(items[i]);
                        if (len > max_w)
                                max_w = len;
                }
                global_ctx.w = max_w + 3;
                global_ctx.h = count;

                if (global_ctx.x + global_ctx.w > term_width)
                {
                        global_ctx.x = term_width - global_ctx.w;
                        if (global_ctx.x < 0)
                                global_ctx.x = 0;
                }

                if (global_ctx.y + global_ctx.h > term_height)
                {
                        global_ctx.h = term_height - global_ctx.y;
                        if (global_ctx.h < 4 && count >= 4)
                        {
                                global_ctx.y = term_height - count;
                                if (global_ctx.y < 0)
                                        global_ctx.y = 0;
                                global_ctx.h = count;
                                if (global_ctx.h > term_height)
                                        global_ctx.h = term_height;
                        }
                }
        }

        ui_rect(global_ctx.x, global_ctx.y, global_ctx.w, global_ctx.h, (Color){15, 15, 15});

        UIListParams params = {
            .x = global_ctx.x, .y = global_ctx.y, .w = global_ctx.w, .h = global_ctx.h, .item_count = count, .cell_w = global_ctx.w, .cell_h = 1, .bg = (Color){15, 15, 15}, .scrollbar_bg = (Color){25, 25, 25}, .scrollbar_fg = (Color){100, 100, 100}};

        ui_list_begin(&global_ctx.list, &params, 0);

        bool action_taken = false;

        for (int i = 0; i < count; i++)
        {
                UIItemResult item;

                if (ui_list_do_item(&global_ctx.list, i, &item))
                {
                        Color bg = (item.hovered || global_ctx.list.selected_idx == i) ? (Color){35, 35, 35} : (Color){15, 15, 15};
                        if (item.pressed)
                                bg = (Color){60, 60, 60};

                        int item_w = params.w - 1;
                        ui_rect(item.x, item.y, item_w, 1, bg);
                        ui_text(item.x + 1, item.y, items[i], (Color){255, 255, 255}, bg, false, false);

                        if (item.clicked)
                        {
                                *out_idx = i;
                                action_taken = true;
                        }
                }
        }

        ui_list_end(&global_ctx.list);

        if ((term_mouse.clicked || term_mouse.right_clicked) && !action_taken && !global_ctx.list.dragging_scroll)
        {
                if (!(term_mouse.x >= global_ctx.x && term_mouse.x < global_ctx.x + params.w &&
                      term_mouse.y >= global_ctx.y && term_mouse.y < global_ctx.y + params.h))
                {
                        global_ctx.active = false;
                        global_ctx.w = 0;
                }
        }

        if (action_taken)
        {
                global_ctx.active = false;
                global_ctx.w = 0;
        }

        return action_taken;
}