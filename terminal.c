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
        char ch;
        Color fg, bg;
} Cell;
typedef struct
{
        int x, y;
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
static int fd_m = -1, raw_mx, raw_my, tc;
static char out_buf[1024 * 1024];

static void on_resize(int s) { resize_flag = 1; }
static bool col_eq(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static int rgb256(Color c) { return 16 + (36 * (c.r * 5 / 255)) + (6 * (c.g * 5 / 255)) + (c.b * 5 / 255); }

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

        char *ct = getenv("COLORTERM");
        tc = ct && (!strcmp(ct, "truecolor") || !strcmp(ct, "24bit"));
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
                canvas[i] = (Cell){' ', {255, 255, 255}, {0, 0, 0}};
}

void ui_rect(int x, int y, int w, int h, Color bg)
{
        for (int r = y; r < y + h; r++)
                for (int c = x; c < x + w; c++)
                        if (c >= 0 && c < term_width && r >= 0 && r < term_height)
                                canvas[r * term_width + c] = (Cell){' ', canvas[r * term_width + c].fg, bg};
}

void ui_text(int x, int y, const char *txt, Color fg, Color bg)
{
        if (y < 0 || y >= term_height)
                return;
        for (int i = 0; txt[i]; i++)
                if (x + i >= 0 && x + i < term_width)
                        canvas[y * term_width + x + i] = (Cell){txt[i], fg, bg};
}

void ui_cursor(void)
{
        if (term_mouse.x >= 0 && term_mouse.x < term_width && term_mouse.y >= 0 && term_mouse.y < term_height)
        {
                Color cc = term_mouse.left ? (Color){0, 255, 0} : (term_mouse.right ? (Color){255, 0, 0} : (Color){255, 255, 0});
                canvas[term_mouse.y * term_width + term_mouse.x].ch = 'X';
                canvas[term_mouse.y * term_width + term_mouse.x].fg = cc;
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
                                if (tc)
                                        printf("\x1b[48;2;%d;%d;%dm", c.bg.r, c.bg.g, c.bg.b);
                                else
                                        printf("\x1b[48;5;%dm", rgb256(c.bg));
                                lbg = c.bg;
                        }
                        if (!col_eq(c.fg, lfg))
                        {
                                if (tc)
                                        printf("\x1b[38;2;%d;%d;%dm", c.fg.r, c.fg.g, c.fg.b);
                                else
                                        printf("\x1b[38;5;%dm", rgb256(c.fg));
                                lfg = c.fg;
                        }
                        putchar(c.ch);
                }
        }
        fflush(stdout);
}