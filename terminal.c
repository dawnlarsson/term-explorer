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

typedef struct
{
        int r, g, b;
} Color;
typedef struct
{
        char ch[5]; // Upgraded for UTF-8 half-blocks
        Color fg, bg;
} Cell;
typedef struct
{
        int x, y;
        int sub_y;    // 0 = top half, 1 = bottom half
        bool has_sub; // True if we have raw pixel-level tracking
        bool left, right, clicked;
        int wheel;
} Mouse;

// Public State
Mouse term_mouse;
int term_width, term_height;

// Private State
static struct termios orig_termios;
static volatile int resize_flag = 1;
static Cell *canvas;
static int fd_m = -1, raw_mx, raw_my;
static int color_mode = 0;
static char out_buf[1024 * 1024];

static void on_resize(int s) { resize_flag = 1; }

static void on_sigint(int s)
{
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[?1006l\x1b[?1015l\x1b[?1003l");
        fflush(stdout);
        _exit(1);
}

static bool col_eq(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static int rgb256(Color c) { return 16 + (36 * (c.r * 5 / 255)) + (6 * (c.g * 5 / 255)) + (c.b * 5 / 255); }

static int rgb_to_ansi16(Color c, bool is_bg)
{
        if (c.r == 0 && c.g == 0 && c.b == 0)
                return is_bg ? 40 : 30;
        if (c.r == 255 && c.g == 255 && c.b == 255)
                return is_bg ? 107 : 97;
        if (c.r == 170 && c.g == 170 && c.b == 170)
                return is_bg ? 47 : 37;
        if (c.r == 255 && c.g == 255 && c.b == 85)
                return is_bg ? 103 : 93;
        if (c.r == 0 && c.g == 255 && c.b == 0)
                return is_bg ? 102 : 92;
        if (c.r == 255 && c.g == 0 && c.b == 0)
                return is_bg ? 101 : 91;
        if (c.r == 255 && c.g == 255 && c.b == 0)
                return is_bg ? 103 : 93;

        int r = c.r > 127 ? 1 : 0;
        int g = c.g > 127 ? 1 : 0;
        int b = c.b > 127 ? 1 : 0;
        int bright = (c.r > 191 || c.g > 191 || c.b > 191) ? 1 : 0;
        int ansi_base = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0);

        return (is_bg ? (bright ? 100 : 40) : (bright ? 90 : 30)) + ansi_base;
}

void term_restore(void)
{
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[?1006l\x1b[?1015l\x1b[?1003l");
        fflush(stdout);
        if (fd_m >= 0)
                close(fd_m);
        if (canvas)
                free(canvas);
}

int term_init(void)
{
        if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
                return 0;
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_cflag |= CS8;
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
                return 0;

        signal(SIGINT, on_sigint);
        signal(SIGTERM, on_sigint);
        signal(SIGQUIT, on_sigint);

        char *ct = getenv("COLORTERM");
        char *term = getenv("TERM");
        if (ct && (!strcmp(ct, "truecolor") || !strcmp(ct, "24bit")))
        {
                color_mode = 2;
        }
        else if (term && strstr(term, "256color"))
        {
                color_mode = 1;
        }
        else
        {
                color_mode = 0;
        }

        fd_m = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);

        setvbuf(stdout, out_buf, _IOFBF, sizeof(out_buf));
        printf("\x1b[?25l\x1b[?1003h\x1b[?1015h\x1b[?1006h");
        signal(SIGWINCH, on_resize);
        return 1;
}

char term_poll(int timeout_ms)
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

        bool last_left = term_mouse.left;
        term_mouse.wheel = 0;
        char key = 0;

        if (fd_m >= 0)
        {
                unsigned char m[3];
                while (read(fd_m, m, 3) == 3)
                {
                        term_mouse.left = m[0] & 1;
                        term_mouse.right = m[0] & 2;
                        raw_mx += (signed char)m[1];
                        raw_my -= (signed char)m[2];
                        if (raw_mx < 0)
                                raw_mx = 0;
                        else if (raw_mx >= term_width * 8)
                                raw_mx = term_width * 8 - 1;
                        if (raw_my < 0)
                                raw_my = 0;
                        else if (raw_my >= term_height * 16)
                                raw_my = term_height * 16 - 1;

                        term_mouse.x = raw_mx / 8;
                        term_mouse.y = raw_my / 16;
                        term_mouse.sub_y = (raw_my / 8) % 2;
                        term_mouse.has_sub = true; // Raw Linux tracking is active
                }
        }

        char buf[64];
        int n = read(STDIN_FILENO, buf, sizeof(buf));
        int i = 0;
        while (i < n)
        {
                if (buf[i] == '\x1b' && i + 2 < n && buf[i + 1] == '[' && buf[i + 2] == '<')
                {
                        int b, x, y, offset = 0;
                        char m_char;
                        if (sscanf(buf + i + 3, "%d;%d;%d%c%n", &b, &x, &y, &m_char, &offset) >= 4)
                        {
                                term_mouse.x = x - 1;
                                term_mouse.y = y - 1;
                                raw_mx = term_mouse.x * 8;
                                raw_my = term_mouse.y * 16;
                                term_mouse.has_sub = false; // ANSI sequence fallback

                                bool down = (m_char == 'M');
                                if (b == 0 || b == 32)
                                        term_mouse.left = down;
                                if (b == 2 || b == 34)
                                        term_mouse.right = down;
                                if (b == 64 && down)
                                        term_mouse.wheel = -1;
                                if (b == 65 && down)
                                        term_mouse.wheel = 1;
                                i += 3 + offset;
                                continue;
                        }
                }
                if (!key)
                        key = buf[i];
                i++;
        }

        term_mouse.clicked = (!last_left && term_mouse.left);
        return key;
}

void ui_begin(void)
{
        static int cw, ch;
        if (term_width != cw || term_height != ch)
        {
                if (canvas)
                        free(canvas);
                canvas = malloc(term_width * term_height * sizeof(Cell)) orelse { exit(1); };
                cw = term_width;
                ch = term_height;
        }
        for (int i = 0; i < cw * ch; i++)
        {
                strcpy(canvas[i].ch, " ");
                canvas[i].fg = (Color){255, 255, 255};
                canvas[i].bg = (Color){0, 0, 0};
        }
}

void ui_rect(int x, int y, int w, int h, Color bg)
{
        for (int r = y; r < y + h; r++)
                for (int c = x; c < x + w; c++)
                        if (c >= 0 && c < term_width && r >= 0 && r < term_height)
                        {
                                int idx = r * term_width + c;
                                strcpy(canvas[idx].ch, " ");
                                canvas[idx].bg = bg;
                        }
}

void ui_text(int x, int y, const char *txt, Color fg, Color bg)
{
        if (y < 0 || y >= term_height)
                return;
        for (int i = 0; txt[i]; i++)
                if (x + i >= 0 && x + i < term_width)
                {
                        int idx = y * term_width + x + i;
                        canvas[idx].ch[0] = txt[i];
                        canvas[idx].ch[1] = '\0';
                        canvas[idx].fg = fg;
                        canvas[idx].bg = bg;
                }
}

void ui_cursor(void)
{
        if (term_mouse.x >= 0 && term_mouse.x < term_width && term_mouse.y >= 0 && term_mouse.y < term_height)
        {
                Color cc = term_mouse.left ? (Color){0, 255, 0} : (term_mouse.right ? (Color){255, 0, 0} : (Color){255, 255, 0});
                int idx = term_mouse.y * term_width + term_mouse.x;

                if (term_mouse.has_sub)
                {
                        if (term_mouse.sub_y == 0)
                                strcpy(canvas[idx].ch, "\xe2\x96\x80"); // ▀ Top
                        else
                                strcpy(canvas[idx].ch, "\xe2\x96\x84"); // ▄ Bottom
                }
                else
                {
                        strcpy(canvas[idx].ch, "\xe2\x96\xa0"); // ■ Solid Block Fallback
                }

                canvas[idx].fg = cc;
        }
}

void ui_end(void)
{
        Color lfg = {-1, -1, -1}, lbg = {-1, -1, -1};
        for (int y = 0; y < term_height; y++)
        {
                printf("\x1b[%d;1H", y + 1);
                for (int x = 0; x < term_width; x++)
                {
                        Cell c = canvas[y * term_width + x];
                        if (!col_eq(c.bg, lbg))
                        {
                                if (color_mode == 2)
                                        printf("\x1b[48;2;%d;%d;%dm", c.bg.r, c.bg.g, c.bg.b);
                                else if (color_mode == 1)
                                        printf("\x1b[48;5;%dm", rgb256(c.bg));
                                else
                                        printf("\x1b[%dm", rgb_to_ansi16(c.bg, true));
                                lbg = c.bg;
                        }
                        if (!col_eq(c.fg, lfg))
                        {
                                if (color_mode == 2)
                                        printf("\x1b[38;2;%d;%d;%dm", c.fg.r, c.fg.g, c.fg.b);
                                else if (color_mode == 1)
                                        printf("\x1b[38;5;%dm", rgb256(c.fg));
                                else
                                        printf("\x1b[%dm", rgb_to_ansi16(c.fg, false));
                                lfg = c.fg;
                        }
                        fputs(c.ch, stdout);
                }
        }
        printf("\x1b[0m");
        fflush(stdout);
}