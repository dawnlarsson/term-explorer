#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
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
        bool has_sub, left, right, clicked;
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
                        if (i + 1 >= n || buf[i + 1] != '[')
                        {
                                key = KEY_ESC;
                                continue;
                        }
                        if (i + 2 < n && buf[i + 2] == '<')
                        {
                                int b, x, y, offset;
                                char m;
                                if (sscanf(buf + i + 3, "%d;%d;%d%c%n", &b, &x, &y, &m, &offset) == 4)
                                {
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
                        else if (i + 5 < n && buf[i + 2] == 'M')
                        {
                                int b = buf[i + 3] - 32, x = buf[i + 4] - 32, y = buf[i + 5] - 32;
                                term_mouse.x = x - 1;
                                term_mouse.y = y - 1;
                                term_mouse.has_sub = false;
                                if (b == 0 || b == 32)
                                        term_mouse.left = true;
                                else if (b == 2 || b == 34)
                                        term_mouse.right = true;
                                else if (b == 3 || b == 35)
                                        term_mouse.left = term_mouse.right = false;
                                else if (b == 64)
                                        term_mouse.wheel--;
                                else if (b == 65)
                                        term_mouse.wheel++;
                                i += 6;
                                continue;
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

void ui_cursor(void)
{
        if (term_mouse.x >= 0 && term_mouse.x < term_width && term_mouse.y >= 0 && term_mouse.y < term_height)
        {
                int idx = term_mouse.y * term_width + term_mouse.x;
                strcpy(canvas[idx].ch, term_mouse.has_sub ? (term_mouse.sub_y == 0 ? "\xe2\x96\x80" : "\xe2\x96\x84") : "\xe2\x96\xa0");
                canvas[idx].fg = term_mouse.left ? (Color){0, 255, 0} : (term_mouse.right ? (Color){255, 0, 0} : (Color){255, 255, 0});
        }
}

void ui_end(void)
{
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
        float target_scroll, current_scroll, scroll_velocity;
        bool dragging_scroll;
        int selected_idx;
        UIListMode mode;
} UIListState;

typedef struct
{
        int x, y, w, h;
        int item_count;
        int cell_w, cell_h;
        Color bg, scrollbar_bg, scrollbar_fg;
} UIListParams;

void ui_list_set_mode(UIListState *s, const UIListParams *p, UIListMode new_mode)
{
        if (s->mode == new_mode)
                return;

        int old_cols = (s->mode == UI_MODE_LIST) ? 1 : ((p->w - 1) / p->cell_w > 0 ? (p->w - 1) / p->cell_w : 1);
        int old_c_h = (s->mode == UI_MODE_LIST) ? 1 : p->cell_h;

        int top_idx = (((int)s->target_scroll + (old_c_h / 2)) / old_c_h) * old_cols;

        s->mode = new_mode;

        int new_cols = (s->mode == UI_MODE_LIST) ? 1 : ((p->w - 1) / p->cell_w > 0 ? (p->w - 1) / p->cell_w : 1);
        int new_c_h = (s->mode == UI_MODE_LIST) ? 1 : p->cell_h;

        s->target_scroll = s->current_scroll = (top_idx / new_cols) * new_c_h;
}

void ui_list_begin(UIListState *s, const UIListParams *p, int key)
{
        int cols = (s->mode == UI_MODE_LIST) ? 1 : ((p->w - 1) / p->cell_w > 0 ? (p->w - 1) / p->cell_w : 1);
        int c_h = (s->mode == UI_MODE_LIST) ? 1 : p->cell_h;
        int rows = (p->item_count + cols - 1) / cols;

        int old_idx = s->selected_idx;
        if (key == KEY_RIGHT && s->selected_idx < p->item_count - 1)
                s->selected_idx++;
        if (key == KEY_LEFT && s->selected_idx > 0)
                s->selected_idx--;
        if (key == KEY_DOWN && s->selected_idx + cols < p->item_count)
                s->selected_idx += cols;
        if (key == KEY_UP && s->selected_idx >= cols)
                s->selected_idx -= cols;
        if (s->selected_idx == -1 && p->item_count > 0 && (key == KEY_RIGHT || key == KEY_LEFT || key == KEY_DOWN || key == KEY_UP))
                s->selected_idx = 0;

        if (old_idx != s->selected_idx && s->selected_idx >= 0)
        {
                int row = s->selected_idx / cols;
                int item_top = row * c_h;
                if (item_top < (int)s->target_scroll)
                        s->target_scroll = (float)item_top;
                else if (item_top + c_h > (int)s->target_scroll + p->h)
                        s->target_scroll = (float)(item_top + c_h - p->h);
        }

        int max_scroll = rows * c_h > p->h ? rows * c_h - p->h : 0;
        int thumb_h = p->h * p->h / (rows * c_h > p->h ? rows * c_h : p->h);
        if (thumb_h < 1)
                thumb_h = 1;

        if (term_mouse.clicked && term_mouse.x == p->x + p->w - 1 && term_mouse.y >= p->y && term_mouse.y < p->y + p->h)
                s->dragging_scroll = true;
        if (!term_mouse.left)
                s->dragging_scroll = false;

        if (s->dragging_scroll && max_scroll > 0)
        {
                float cr = (float)(term_mouse.y - p->y - (thumb_h / 2)) / (p->h - thumb_h > 0 ? p->h - thumb_h : 1);
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

bool ui_list_do_item(UIListState *s, const UIListParams *p, int index, int *out_x, int *out_y, bool *is_hovered, bool *is_pressed)
{
        int cols = (s->mode == UI_MODE_LIST) ? 1 : ((p->w - 1) / p->cell_w > 0 ? (p->w - 1) / p->cell_w : 1);
        int c_w = (s->mode == UI_MODE_LIST) ? p->w - 1 : p->cell_w;
        int c_h = (s->mode == UI_MODE_LIST) ? 1 : p->cell_h;

        int screen_x = p->x + (index % cols) * c_w;
        int screen_y = p->y + (index / cols * c_h) - (int)(s->current_scroll + 0.5f);

        if (screen_y + c_h <= p->y || screen_y >= p->y + p->h)
                return false;

        *out_x = screen_x;
        *out_y = screen_y;
        *is_hovered = (!s->dragging_scroll && term_mouse.x >= screen_x && term_mouse.x < screen_x + c_w &&
                       term_mouse.y >= screen_y && term_mouse.y < screen_y + c_h &&
                       term_mouse.y >= p->y && term_mouse.y < p->y + p->h && term_mouse.x < p->x + p->w - 1);
        *is_pressed = (*is_hovered && term_mouse.left);

        if (*is_hovered && term_mouse.clicked)
                s->selected_idx = index;
        return true;
}

void ui_list_end(UIListState *s, const UIListParams *p)
{
        int cols = (s->mode == UI_MODE_LIST) ? 1 : ((p->w - 1) / p->cell_w > 0 ? (p->w - 1) / p->cell_w : 1);
        int c_h = (s->mode == UI_MODE_LIST) ? 1 : p->cell_h;
        int rows = (p->item_count + cols - 1) / cols;
        int max_scroll = rows * c_h > p->h ? rows * c_h - p->h : 0;

        if (max_scroll > 0)
        {
                int thumb_h = p->h * p->h / (rows * c_h);
                if (thumb_h < 1)
                        thumb_h = 1;
                ui_rect(p->x + p->w - 1, p->y, 1, p->h, p->scrollbar_bg);

                Color thumb_col = s->dragging_scroll ? (Color){255, 255, 255} : p->scrollbar_fg;
                ui_rect(p->x + p->w - 1, p->y + (int)((s->current_scroll / max_scroll) * (p->h - thumb_h)), 1, thumb_h, thumb_col);
        }
}