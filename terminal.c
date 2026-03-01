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

#define ANIM_SPEED_CARRY 0.4f
#define ANIM_SPEED_DROP 0.10f
#define ANIM_SPEED_FLY 0.15f
#define ANIM_SPEED_POP 0.08f

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
typedef enum
{
        UI_MODE_GRID,
        UI_MODE_LIST
} UIListMode;

typedef struct
{
        int x, y, w, h, item_count, cell_w, cell_h;
        Color bg, scrollbar_bg, scrollbar_fg;
} UIListParams;

typedef struct
{
        float target_scroll, current_scroll, scroll_velocity, drag_offset, kb_drag_x, kb_drag_y, box_start_y_world;
        bool dragging_scroll, clicked_on_item, is_dragging, is_kb_dragging, is_box_selecting, ignore_mouse;
        int selected_idx, last_nav_key, nav_key_streak, drag_idx, drag_start_x, drag_start_y, drag_off_x, drag_off_y;
        int drop_target_idx, action_drop_src, action_drop_dst, action_click_idx, kb_drag_idx, box_start_x, selections_cap;
        int last_mouse_x, last_mouse_y;
        long long last_nav_time;
        UIListMode mode;
        UIListParams p;
        bool *selections, *active_box_selections;

        float carry_x, carry_y, pickup_anim, drop_anim, fly_anim;
        int fly_origin_x, fly_origin_y, drop_dst_x, drop_dst_y;

        bool fly_is_pickup, drop_to_target, carrying;
} UIListState;

typedef struct
{
        bool active;
        int x, y, w, h;
        UIListState list;
} UIContextState;
typedef struct
{
        int x, y, w, h;
        bool hovered, pressed, clicked, right_clicked, is_ghost, is_drop_target, is_selected;
} UIItemResult;

typedef struct
{
        int x, y, w, h;
} UIRect;

typedef void (*UIActionFn)(void *payload);

typedef struct
{
        UIActionFn undo_fn;
        UIActionFn redo_fn;
        UIActionFn free_fn;
        void *payload;
} UIAction;

typedef struct
{
        UIAction stack[512];
        int head, count;
} UIHistory;

UIHistory global_history;

Mouse term_mouse;
int term_width, term_height, term_anim_timeout = 16;
float term_dt_scale = 1.0f;

static struct termios orig_termios;
static volatile int resize_flag = 1;
static Cell *canvas, *last_canvas;
static int fd_m = -1, raw_mx, raw_my, color_mode;
static bool is_evdev;
static char out_buf[1024 * 1024];
static UIContextState global_ctx;
static int global_ctx_target = -1;

typedef struct
{
        float x, y, vx, vy, life;
        Color color;
} UIParticle;

UIParticle term_particles[1024];
int term_particle_count = 0;

void ui_spawn_particle(float x, float y, float vx, float vy, Color c, float life)
{
        if (term_particle_count < 1024)
                term_particles[term_particle_count++] = (UIParticle){x, y, vx, vy, life, c};
}

void ui_burst_particles(int cx, int cy, int count, Color c)
{
        for (int i = 0; i < count; i++)
        {
                float vx = ((rand() % 100) / 50.0f) - 1.0f;
                float vy = ((rand() % 100) / 50.0f) - 2.0f;

                ui_spawn_particle((float)cx, (float)(cy * 2 + 1), vx * 2.0f, vy * 1.5f, c, 10.0f + (rand() % 15));
        }
}

void ui_scale_region(int x, int y, int w, int h, float scale, Color clear_bg)
{
        if (scale >= 0.99f)
                return;
        if (scale <= 0.01f)
                scale = 0.01f;
        if (w * h > 2048)
                return;

        raw Cell temp[2048];
        for (int r = 0; r < h; r++)
        {
                for (int c = 0; c < w; c++)
                {
                        int cx = x + c, cy = y + r;
                        if (cx >= 0 && cx < term_width && cy >= 0 && cy < term_height)
                                temp[r * w + c] = canvas[cy * term_width + cx];
                        else
                                temp[r * w + c] = (Cell){" ", clear_bg, clear_bg, false, false};
                }
        }

        for (int r = 0; r < h; r++)
        {
                for (int c = 0; c < w; c++)
                {
                        int cx = x + c, cy = y + r;
                        if (cx >= 0 && cx < term_width && cy >= 0 && cy < term_height)
                                canvas[cy * term_width + cx] = (Cell){" ", clear_bg, clear_bg, false, false};
                }
        }

        int nw = (int)(w * scale), nh = (int)(h * scale);
        if (nw == 0)
                nw = 1;
        if (nh == 0)
                nh = 1;

        int nx = x + (w - nw) / 2, ny = y + (h - nh) / 2;

        for (int r = 0; r < nh; r++)
        {
                for (int c = 0; c < nw; c++)
                {
                        int src_r = (int)(r / scale), src_c = (int)(c / scale);
                        if (src_r >= h)
                                src_r = h - 1;
                        if (src_c >= w)
                                src_c = w - 1;

                        int cx = nx + c, cy = ny + r;
                        if (cx >= 0 && cx < term_width && cy >= 0 && cy < term_height)
                                canvas[cy * term_width + cx] = temp[src_r * w + src_c];
                }
        }
}

UIRect ui_list_item_rect(const UIListState *s, int index)
{
        int cols = (s->mode == UI_MODE_LIST) ? 1 : ((s->p.w - 1) / s->p.cell_w > 0 ? (s->p.w - 1) / s->p.cell_w : 1);
        int c_w = (s->mode == UI_MODE_LIST) ? s->p.w - 1 : s->p.cell_w;
        int c_h = (s->mode == UI_MODE_LIST) ? 1 : s->p.cell_h;
        return (UIRect){
            .x = s->p.x + (index % cols) * c_w,
            .y = s->p.y + (index / cols * c_h) - (int)(s->current_scroll + 0.5f),
            .w = c_w,
            .h = c_h};
}

void ui_action_push(UIActionFn undo, UIActionFn redo, UIActionFn free_fn, void *payload)
{
        if (global_history.head < global_history.count)
        {
                for (int i = global_history.head; i < global_history.count; i++)
                        if (global_history.stack[i].free_fn)
                                global_history.stack[i].free_fn(global_history.stack[i].payload);
        }

        if (global_history.head >= 512)
        {
                if (global_history.stack[0].free_fn)
                        global_history.stack[0].free_fn(global_history.stack[0].payload);
                memmove(global_history.stack, global_history.stack + 1, 511 * sizeof(UIAction));
                global_history.head = 511;
        }

        global_history.stack[global_history.head++] = (UIAction){undo, redo, free_fn, payload};
        global_history.count = global_history.head;
}

bool ui_action_undo(void)
{
        (global_history.head > 0) orelse return false;
        global_history.head--;
        UIAction *act = &global_history.stack[global_history.head];
        if (act->undo_fn)
                act->undo_fn(act->payload);
        return true;
}

bool ui_action_redo(void)
{
        (global_history.head < global_history.count) orelse return false;
        UIAction *act = &global_history.stack[global_history.head];
        global_history.head++;
        if (act->redo_fn)
                act->redo_fn(act->payload);
        return true;
}

void ui_action_clear(void)
{
        for (int i = 0; i < global_history.count; i++)
                if (global_history.stack[i].free_fn)
                        global_history.stack[i].free_fn(global_history.stack[i].payload);
        global_history.head = global_history.count = 0;
}

static void on_resize(int s) { resize_flag = 1; }
static void on_sigint(int s)
{
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\033%%@\033[0m\033[2J\033[H\033[?25h\033[?7h\033[?1006l\033[?1015l\033[?1003l");
        fflush(stdout);
        _exit(1);
}

static float ui_fabsf(float v) { return v < 0.0f ? -v : v; }
static float ui_powf(float base, float exp)
{
        float res = 1.0f;
        for (int i = 0; i < (int)exp; i++)
                res *= base;
        return res * (1.0f + (exp - (int)exp) * (base - 1.0f));
}

static bool col_eq(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static bool cell_eq(const Cell *a, const Cell *b)
{
        if (a->bold != b->bold || a->invert != b->invert)
                return false;
        if (a->bg.r != b->bg.r || a->bg.g != b->bg.g || a->bg.b != b->bg.b)
                return false;
        if (a->fg.r != b->fg.r || a->fg.g != b->fg.g || a->fg.b != b->fg.b)
                return false;
        return strcmp(a->ch, b->ch) == 0;
}

static int rgb256(Color c) { return 16 + (36 * (c.r * 5 / 255)) + (6 * (c.g * 5 / 255)) + (c.b * 5 / 255); }
static int rgb_to_ansi16(Color c, bool is_bg)
{
        int r = c.r > 127 ? 1 : 0, g = c.g > 127 ? 1 : 0, b = c.b > 127 ? 1 : 0;
        int bright = (c.r > 170 || c.g > 170 || c.b > 170) ? 1 : 0;

        if (!r && !g && !b && (c.r || c.g || c.b))
        {
                if (c.r == c.g && c.g == c.b)
                {
                        r = g = b = 0;
                        bright = 1;
                }
                else if (c.r >= c.g && c.r >= c.b)
                        r = 1;
                else if (c.g >= c.r && c.g >= c.b)
                        g = 1;
                else
                        b = 1;
        }
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
        free(canvas);
        free(last_canvas);
}

int term_init(void)
{
        (tcgetattr(STDIN_FILENO, &orig_termios) == 0) orelse return 0;
        struct termios t_raw = orig_termios;
        t_raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        t_raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        t_raw.c_oflag &= ~(OPOST);
        t_raw.c_cflag |= CS8;
        t_raw.c_cc[VMIN] = t_raw.c_cc[VTIME] = 0;
        (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t_raw) == 0) orelse return 0;

        signal(SIGINT, on_sigint);
        signal(SIGTERM, on_sigint);
        signal(SIGQUIT, on_sigint);

        char *ct = getenv("COLORTERM");
        char *term = getenv("TERM");
        color_mode = (ct && (!strcmp(ct, "truecolor") || !strcmp(ct, "24bit"))) ? 2 : (term && strstr(term, "256color")) ? 1
                                                                                                                         : 0;

#ifdef __linux__
        for (int i = 0; i < 32 && fd_m < 0; i++)
        {
                raw char path[64];
                snprintf(path, sizeof(path), "/dev/input/event%d", i);
                int fd = open(path, O_RDONLY | O_NONBLOCK);
                (fd >= 0) orelse continue;

                raw unsigned long ev[EV_MAX / 8 + 1];
                raw unsigned long rel[REL_MAX / 8 + 1];
                ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
                ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);

                if ((ev[EV_REL / 8] & (1 << (EV_REL % 8))) && (rel[REL_X / 8] & (1 << (REL_X % 8))) && (rel[REL_Y / 8] & (1 << (REL_Y % 8))))
                {
                        fd_m = fd;
                        is_evdev = true;
                }
                else
                {
                        close(fd);
                }
        }
#endif
        if (fd_m < 0)
                fd_m = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);

        setvbuf(stdout, out_buf, _IOFBF, sizeof(out_buf));
        printf("\033%%G\033[?25l\033[?7l\033[?1003h\033[?1015h\033[?1006h");

        char *fps_env = getenv("FPS");
        int target_fps = fps_env ? atoi(fps_env) : 60;
        term_anim_timeout = 1000 / (target_fps < 24 ? 60 : target_fps);
        term_dt_scale = 60.0f / (target_fps < 24 ? 60 : target_fps);

        signal(SIGWINCH, on_resize);
        return 1;
}
int term_poll(int timeout_ms)
{
        struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {fd_m, POLLIN, 0}};
        poll(fds, fd_m >= 0 ? 2 : 1, timeout_ms);

        if (resize_flag)
        {
                raw struct winsize ws;
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
                term_mouse = (Mouse){term_width / 2, term_height / 2, 0, 0, false, false, false, false, false, true};
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
                        raw struct input_event ev;
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
                        raw unsigned char m[3];
                        while (read(fd_m, m, 3) == 3)
                        {
                                term_mouse.hide_cursor = false;
                                term_mouse.left = m[0] & 1;
                                term_mouse.right = m[0] & 2;
                                raw_mx += (signed char)m[1];
                                raw_my -= (signed char)m[2];
                        }
                }
                raw_mx = raw_mx < 0 ? 0 : (raw_mx >= term_width * 8 ? term_width * 8 - 1 : raw_mx);
                raw_my = raw_my < 0 ? 0 : (raw_my >= term_height * 16 ? term_height * 16 - 1 : raw_my);
                term_mouse.x = raw_mx / 8;
                term_mouse.y = raw_my / 16;
                term_mouse.sub_y = (raw_my / 8) % 2;
                term_mouse.has_sub = true;
        }

        while (1)
        {
                raw char buf[4096];
                int n = read(STDIN_FILENO, buf, sizeof(buf));
                (n > 0) orelse break;

                for (int i = 0; i < n; i++)
                {
                        if (buf[i] == '\033')
                        {
                                int b, x, y, offset;
                                char m;
                                if (i + 2 < n && buf[i + 2] == '<' && sscanf(buf + i + 3, "%d;%d;%d%c%n", &b, &x, &y, &m, &offset) == 4)
                                {
                                        term_mouse = (Mouse){x - 1, y - 1, 0, term_mouse.wheel, false, term_mouse.left, term_mouse.right, false, false, true};
                                        bool d = (m == 'M');
                                        if (m == 'm' || b == 3 || b == 35)
                                                term_mouse.left = term_mouse.right = false;
                                        else if (b == 0 || b == 32)
                                                term_mouse.left = d;
                                        else if (b == 2 || b == 34)
                                                term_mouse.right = d;
                                        if (b == 64 && d)
                                                term_mouse.wheel--;
                                        if (b == 65 && d)
                                                term_mouse.wheel++;
                                        i += 3 + offset;
                                        continue;
                                }
                                else if (i + 5 < n && buf[i + 5] == '~' && buf[i + 3] == ';' && buf[i + 4] == '2')
                                {
                                        key = (buf[i + 2] == '5') ? KEY_SHIFT_PAGE_UP : (buf[i + 2] == '6') ? KEY_SHIFT_PAGE_DOWN
                                                                                                            : 0;
                                        if (key)
                                        {
                                                i += 5;
                                                continue;
                                        }
                                }
                                else if (i + 3 < n && buf[i + 3] == '~')
                                {
                                        char c = buf[i + 2];
                                        key = (c == '5') ? KEY_PAGE_UP : (c == '6')           ? KEY_PAGE_DOWN
                                                                     : (c == '4' || c == '8') ? KEY_END
                                                                     : (c == '1' || c == '7') ? KEY_HOME
                                                                                              : 0;
                                        if (key)
                                        {
                                                i += 3;
                                                continue;
                                        }
                                }
                                else if (i + 2 < n && buf[i + 1] == 'O')
                                {
                                        key = (buf[i + 2] == 'F') ? KEY_END : (buf[i + 2] == 'H') ? KEY_HOME
                                                                                                  : 0;
                                        if (key)
                                        {
                                                i += 2;
                                                continue;
                                        }
                                }
                                else if (i + 2 < n)
                                {
                                        char c = buf[i + 2];
                                        key = (c == 'A') ? KEY_UP : (c == 'B') ? KEY_DOWN
                                                                : (c == 'C')   ? KEY_RIGHT
                                                                : (c == 'D')   ? KEY_LEFT
                                                                : (c == 'F')   ? KEY_END
                                                                : (c == 'H')   ? KEY_HOME
                                                                               : 0;
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
        }

        term_mouse.clicked = (!last_left && term_mouse.left);
        term_mouse.right_clicked = (!last_right && term_mouse.right);
        return key;
}

void ui_begin(void)
{
        static long long last_time;
        raw struct timeval tv;
        gettimeofday(&tv, NULL);
        long long now_us = (long long)tv.tv_sec * 1000000 + tv.tv_usec;

        if (last_time != 0)
        {
                float dt_ms = (now_us - last_time) / 1000.0f;
                term_dt_scale = (dt_ms > 33.0f ? 16.666f : (dt_ms <= 0.0f ? 1.0f : dt_ms)) / 16.666f;
        }
        last_time = now_us;

        static int cw, ch;
        if (term_width != cw || term_height != ch)
        {
                canvas = realloc(canvas, term_width * term_height * sizeof(Cell)) orelse { exit(1); };
                last_canvas = realloc(last_canvas, term_width * term_height * sizeof(Cell)) orelse { exit(1); };
                memset(last_canvas, 0, term_width * term_height * sizeof(Cell));
                printf("\033[2J\033[H");
                fflush(stdout);
                cw = term_width;
                ch = term_height;
        }
}

void ui_clear(Color bg)
{
        for (int i = 0; i < term_width * term_height; i++)
                canvas[i] = (Cell){" ", bg, bg, false, false};
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
                        canvas[r * term_width + c] = (Cell){" ", bg, bg, false, false};
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
                        *c = (Cell){"", fg, bg, bold, invert};
                        for (int j = 0; j < len && txt[i + j]; j++)
                                c->ch[actual_len++] = txt[i + j];
                }
                else
                        for (int j = 0; j < len && txt[i + j]; j++)
                                actual_len++;
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

void ui_end(void)
{
        for (int i = 0; i < term_particle_count; i++)
        {
                UIParticle *p = &term_particles[i];
                p->x += p->vx * term_dt_scale;
                p->y += p->vy * term_dt_scale;
                p->vy += 0.2f * term_dt_scale;
                p->life -= term_dt_scale;

                if (p->life <= 0.0f)
                {
                        term_particles[i] = term_particles[--term_particle_count];
                        i--;
                        continue;
                }

                int px = (int)p->x;
                int py = (int)p->y;
                if (px >= 0 && px < term_width && py >= 0 && py < term_height * 2)
                {
                        Cell *c = &canvas[(py / 2) * term_width + px];
                        c->fg = p->color;
                        strcpy(c->ch, (py % 2 == 0) ? "\xe2\x96\x80" : "\xe2\x96\x84");
                }
        }

        if (!term_mouse.hide_cursor && term_mouse.x >= 0 && term_mouse.x < term_width && term_mouse.y >= 0 && term_mouse.y < term_height)
        {
                int idx = term_mouse.y * term_width + term_mouse.x;
                strcpy(canvas[idx].ch, term_mouse.has_sub ? (term_mouse.sub_y == 0 ? "\xe2\x96\x80" : "\xe2\x96\x84") : "\xe2\x96\xa0");
                canvas[idx].fg = term_mouse.left ? (Color){0, 255, 0} : (term_mouse.right ? (Color){255, 0, 0} : (Color){255, 255, 0});
        }

        Color lfg = {-1, -1, -1}, lbg = {-1, -1, -1};
        bool lbold = false, linvert = false;
        int last_x = -1, last_y = -1;

        printf("\033[?2026h");
        for (int y = 0; y < term_height; y++)
        {
                for (int x = 0; x < term_width; x++)
                {
                        int i = y * term_width + x;
                        Cell *c = &canvas[i], *lc = &last_canvas[i];
                        if (cell_eq(c, lc))
                                continue;

                        if (x != last_x + 1 || y != last_y)
                                printf("\033[%d;%dH", y + 1, x + 1);
                        last_x = x;
                        last_y = y;

                        if (c->bold != lbold)
                        {
                                printf(c->bold ? "\033[1m" : "\033[22m");
                                lbold = c->bold;
                        }
                        if (c->invert != linvert)
                        {
                                printf(c->invert ? "\033[7m" : "\033[27m");
                                linvert = c->invert;
                        }
                        if (!col_eq(c->bg, lbg))
                        {
                                SET_COL(0, c->bg);
                                lbg = c->bg;
                        }
                        if (!col_eq(c->fg, lfg))
                        {
                                SET_COL(1, c->fg);
                                lfg = c->fg;
                        }

                        fputs(c->ch, stdout);
                        *lc = *c;
                }
        }
        printf("\x1b[0m\033[?2026l");
        fflush(stdout);
}

void ui_list_clear_selections(UIListState *s)
{
        if (s->selections_cap > 0)
        {
                memset(s->selections, 0, s->selections_cap * sizeof(bool));
                memset(s->active_box_selections, 0, s->selections_cap * sizeof(bool));
        }
}

void ui_list_reset(UIListState *s)
{
        bool *sel = s->selections, *box = s->active_box_selections;
        int cap = s->selections_cap;

        float carry_x = s->carry_x, carry_y = s->carry_y, pickup_anim = s->pickup_anim, drop_anim = s->drop_anim, fly_anim = s->fly_anim;
        int fly_origin_x = s->fly_origin_x, fly_origin_y = s->fly_origin_y, drop_dst_x = s->drop_dst_x, drop_dst_y = s->drop_dst_y;
        bool fly_is_pickup = s->fly_is_pickup, drop_to_target = s->drop_to_target, carrying = s->carrying;

        *s = (UIListState){0};

        s->selections = sel;
        s->active_box_selections = box;
        s->selections_cap = cap;

        s->carry_x = carry_x;
        s->carry_y = carry_y;
        s->pickup_anim = pickup_anim;
        s->drop_anim = drop_anim;
        s->fly_anim = fly_anim;
        s->fly_origin_x = fly_origin_x;
        s->fly_origin_y = fly_origin_y;
        s->drop_dst_x = drop_dst_x;
        s->drop_dst_y = drop_dst_y;
        s->fly_is_pickup = fly_is_pickup;
        s->drop_to_target = drop_to_target;
        s->carrying = carrying;

        s->selected_idx = s->drag_idx = s->drop_target_idx = s->action_drop_src = s->action_drop_dst = s->action_click_idx = s->kb_drag_idx = -1;
        ui_list_clear_selections(s);
}

void ui_list_set_mode(UIListState *s, const UIListParams *p, UIListMode mode)
{
        if (s->mode == mode)
                return;
        s->mode = mode;
        if (s->selected_idx >= 0)
        {
                int cols = (mode == UI_MODE_LIST) ? 1 : ((p->w - 1) / p->cell_w > 0 ? (p->w - 1) / p->cell_w : 1);
                s->target_scroll = s->current_scroll = (float)((s->selected_idx / cols) * (mode == UI_MODE_LIST ? 1 : p->cell_h));
        }
}
void ui_list_begin(UIListState *s, const UIListParams *p, int key)
{
        s->p = *p;
        s->clicked_on_item = false;
        s->action_drop_src = -1;
        s->action_drop_dst = -1;
        s->action_click_idx = -1;
        s->drop_target_idx = -1;

        if (term_mouse.x != s->last_mouse_x || term_mouse.y != s->last_mouse_y)
        {
                s->ignore_mouse = false;
                s->last_mouse_x = term_mouse.x;
                s->last_mouse_y = term_mouse.y;
        }

        if (key >= KEY_UP && key <= KEY_SHIFT_PAGE_DOWN)
                s->ignore_mouse = true;

        if (p->item_count > s->selections_cap)
        {
                s->selections = realloc(s->selections, p->item_count * sizeof(bool)) orelse { exit(1); };
                s->active_box_selections = realloc(s->active_box_selections, p->item_count * sizeof(bool)) orelse { exit(1); };
                memset(s->selections + s->selections_cap, 0, (p->item_count - s->selections_cap) * sizeof(bool));
                memset(s->active_box_selections + s->selections_cap, 0, (p->item_count - s->selections_cap) * sizeof(bool));
                s->selections_cap = p->item_count;
        }

        if (s->selections_cap > 0)
                memset(s->active_box_selections, 0, s->selections_cap * sizeof(bool));
        if (key == ' ' && s->selected_idx >= 0 && s->selected_idx < p->item_count)
                s->selections[s->selected_idx] = !s->selections[s->selected_idx];

        if (key == KEY_ESC)
        {
                ui_list_clear_selections(s);
                s->is_kb_dragging = false;
                s->kb_drag_idx = -1;
        }

        if (key == '\t')
        {
                if (!s->is_kb_dragging && s->selected_idx != -1)
                {
                        s->is_kb_dragging = true;
                        s->kb_drag_idx = s->selected_idx;
                        int cols = (s->mode == UI_MODE_LIST) ? 1 : ((s->p.w - 1) / s->p.cell_w > 0 ? (s->p.w - 1) / s->p.cell_w : 1);
                        s->kb_drag_x = s->p.x + (s->selected_idx % cols) * ((s->mode == UI_MODE_LIST) ? s->p.w - 1 : s->p.cell_w);
                        s->kb_drag_y = s->p.y + (s->selected_idx / cols * ((s->mode == UI_MODE_LIST) ? 1 : s->p.cell_h)) - s->current_scroll;
                }
                else if (s->is_kb_dragging)
                {
                        s->is_kb_dragging = false;
                        s->kb_drag_idx = -1;
                }
        }

        if (key == KEY_ENTER && s->is_kb_dragging)
        {
                if (s->selected_idx != -1 && s->selected_idx != s->kb_drag_idx)
                {
                        s->action_drop_src = s->kb_drag_idx;
                        s->action_drop_dst = s->selected_idx;
                }
                s->is_kb_dragging = false;
                s->kb_drag_idx = -1;
        }

        int cols = (s->mode == UI_MODE_LIST) ? 1 : ((s->p.w - 1) / s->p.cell_w > 0 ? (s->p.w - 1) / s->p.cell_w : 1);
        int c_h = (s->mode == UI_MODE_LIST) ? 1 : s->p.cell_h;
        int rows = (s->p.item_count + cols - 1) / cols;
        int items_per_page = (s->p.h / c_h > 0 ? s->p.h / c_h : 1) * cols;

        raw struct timeval tv;
        gettimeofday(&tv, NULL);
        long long now_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;

        if (key >= KEY_UP && key <= KEY_LEFT)
        {
                if (key == s->last_nav_key && (now_ms - s->last_nav_time) < 400)
                        s->nav_key_streak++;
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

        int step = (s->nav_key_streak > 30) ? ((s->mode == UI_MODE_LIST) ? 25 : 8) : (s->nav_key_streak > 15) ? ((s->mode == UI_MODE_LIST) ? 10 : 4)
                                                                                 : (s->nav_key_streak > 5)    ? ((s->mode == UI_MODE_LIST) ? 3 : 2)
                                                                                                              : 1;
        int old_idx = s->selected_idx;

        if (s->selected_idx == -1 && s->p.item_count > 0 && (key >= KEY_UP && key <= KEY_SHIFT_PAGE_DOWN))
                s->selected_idx = 0;
        else if (s->selected_idx >= 0)
        {
                if (key == KEY_RIGHT)
                        s->selected_idx = (s->selected_idx + step >= s->p.item_count) ? s->p.item_count - 1 : s->selected_idx + step;
                if (key == KEY_LEFT)
                        s->selected_idx = (s->selected_idx - step < 0) ? 0 : s->selected_idx - step;
                if (key == KEY_DOWN)
                        s->selected_idx = (s->selected_idx + cols * step >= s->p.item_count) ? s->p.item_count - 1 : s->selected_idx + cols * step;
                if (key == KEY_UP)
                        s->selected_idx = (s->selected_idx - cols * step < 0) ? old_idx % cols : s->selected_idx - cols * step;
                if (key == KEY_PAGE_DOWN)
                        s->selected_idx = (s->selected_idx + items_per_page >= s->p.item_count) ? s->p.item_count - 1 : s->selected_idx + items_per_page;
                if (key == KEY_PAGE_UP)
                        s->selected_idx = (s->selected_idx - items_per_page < 0) ? old_idx % cols : s->selected_idx - items_per_page;
                if (key == KEY_END || key == KEY_SHIFT_PAGE_DOWN)
                        s->selected_idx = s->p.item_count - 1;
                if (key == KEY_HOME || key == KEY_SHIFT_PAGE_UP)
                        s->selected_idx = 0;
        }

        if (old_idx != s->selected_idx && s->selected_idx >= 0)
        {
                int item_top = (s->selected_idx / cols) * c_h;
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
                s->drag_offset = (term_mouse.y >= thumb_top && term_mouse.y < thumb_top + thumb_h) ? (float)(term_mouse.y - thumb_top) : (float)thumb_h / 2.0f;
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

        if (term_mouse.wheel != 0)
        {
                float base_power = (s->mode == UI_MODE_LIST) ? 0.18f : ((float)c_h * 0.1f);
                float wheel_dir = term_mouse.wheel > 0 ? 1.0f : -1.0f;
                float vel_dir = s->scroll_velocity > 0 ? 1.0f : (s->scroll_velocity < 0 ? -1.0f : 0.0f);

                if (vel_dir != 0.0f && vel_dir != wheel_dir)
                        s->scroll_velocity = 0.0f;
                if (vel_dir == wheel_dir && ui_fabsf(s->scroll_velocity) > 0.05f)
                        base_power *= (2.5f + ui_fabsf(s->scroll_velocity) * 1.2f);
                s->scroll_velocity += term_mouse.wheel * base_power;
        }

        s->target_scroll += s->scroll_velocity * term_dt_scale;
        s->scroll_velocity *= ui_powf(0.82f, term_dt_scale);

        if (s != &global_ctx.list && (s->dragging_scroll || term_mouse.wheel != 0))
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
                float snap = (float)(((int)(s->target_scroll + (c_h / 2.0f)) / c_h) * c_h);
                if (snap <= max_scroll)
                        s->target_scroll = snap;
        }

        if (!s->dragging_scroll)
        {
                s->current_scroll += (s->target_scroll - s->current_scroll) * (1.0f - ui_powf(0.7f, term_dt_scale));
                if (s->target_scroll - s->current_scroll > -0.05f && s->target_scroll - s->current_scroll < 0.05f)
                        s->current_scroll = s->target_scroll;
        }
}

bool ui_list_do_item(UIListState *s, int index, UIItemResult *res)
{
        UIRect r = ui_list_item_rect(s, index);
        float item_world_y = s->p.y + (index / (s->mode == UI_MODE_LIST ? 1 : ((s->p.w - 1) / s->p.cell_w > 0 ? (s->p.w - 1) / s->p.cell_w : 1)) * r.h);

        if (s->is_box_selecting)
        {
                int bx = s->box_start_x < term_mouse.x ? s->box_start_x : term_mouse.x;
                int bw = abs(term_mouse.x - s->box_start_x) + 1;
                float world_by = s->box_start_y_world < (term_mouse.y + s->current_scroll) ? s->box_start_y_world : (term_mouse.y + s->current_scroll);
                float world_bh = ui_fabsf((term_mouse.y + s->current_scroll) - s->box_start_y_world) + 1;

                if (r.x < bx + bw && r.x + r.w > bx && item_world_y < world_by + world_bh && item_world_y + r.h > world_by)
                        s->active_box_selections[index] = true;
        }

        if (r.y + r.h <= s->p.y || r.y >= s->p.y + s->p.h)
                return false;
        *res = (UIItemResult){.x = r.x, .y = r.y, .w = r.w, .h = r.h};

        bool over_ctx_menu = global_ctx.active && (s != &global_ctx.list) && term_mouse.x >= global_ctx.x && term_mouse.x < global_ctx.x + global_ctx.w && term_mouse.y >= global_ctx.y && term_mouse.y < global_ctx.y + global_ctx.h;
        res->hovered = (!s->ignore_mouse && !over_ctx_menu && !s->dragging_scroll && !s->is_box_selecting && term_mouse.x >= r.x && term_mouse.x < r.x + r.w && term_mouse.y >= r.y && term_mouse.y < r.y + r.h && term_mouse.y >= s->p.y && term_mouse.y < s->p.y + s->p.h && term_mouse.x < s->p.x + s->p.w - 1);

        bool is_hit = (s->mode == UI_MODE_GRID) ? !(term_mouse.x == r.x || term_mouse.x == r.x + r.w - 1 || term_mouse.y == r.y || term_mouse.y == r.y + r.h - 1) : (term_mouse.x <= r.x + 35);

        res->pressed = (res->hovered && term_mouse.left);
        res->clicked = (res->hovered && term_mouse.clicked);
        res->right_clicked = (res->hovered && term_mouse.right_clicked);

        if (res->clicked || res->right_clicked)
        {
                s->selected_idx = index;
                if (is_hit)
                        s->clicked_on_item = true;
        }

        if (res->hovered && term_mouse.clicked && is_hit && s->drag_idx == -1 && !s->dragging_scroll && !global_ctx.active)
        {
                s->drag_idx = index;
                s->drag_start_x = term_mouse.x;
                s->drag_start_y = term_mouse.y;
                s->drag_off_x = term_mouse.x - r.x;
                s->drag_off_y = term_mouse.y - r.y;
        }

        res->is_selected = s->selections[index] || s->active_box_selections[index];
        res->is_ghost = (s->is_dragging && s->drag_idx == index) || (s->is_kb_dragging && s->kb_drag_idx == index);
        res->is_drop_target = (s->is_dragging && res->hovered && index != s->drag_idx) || (s->is_kb_dragging && s->selected_idx == index && index != s->kb_drag_idx);

        if (res->is_drop_target)
                s->drop_target_idx = index;
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
                if (term_mouse.x >= s->p.x && term_mouse.x < s->p.x + s->p.w - 1 && term_mouse.y >= s->p.y && term_mouse.y < s->p.y + s->p.h)
                {
                        s->selected_idx = -1;
                        ui_list_clear_selections(s);
                        s->is_box_selecting = true;
                        s->box_start_x = term_mouse.x;
                        s->box_start_y_world = term_mouse.y + s->current_scroll;
                }
        }

        if (s->is_box_selecting)
        {
                int start_screen_y = (int)(s->box_start_y_world - s->current_scroll);
                int curr_screen_y = term_mouse.y;
                int bx = s->box_start_x < term_mouse.x ? s->box_start_x : term_mouse.x;
                int by = start_screen_y < curr_screen_y ? start_screen_y : curr_screen_y;
                int bw = abs(term_mouse.x - s->box_start_x) + 1;
                int bh = abs(curr_screen_y - start_screen_y) + 1;
                Color box_clr = {60, 100, 180};

                for (int x = bx; x < bx + bw; x++)
                {
                        if (x >= 0 && x < term_width && by >= s->p.y && by < s->p.y + s->p.h)
                                canvas[by * term_width + x].bg = box_clr;
                        if (x >= 0 && x < term_width && by + bh - 1 >= s->p.y && by + bh - 1 < s->p.y + s->p.h)
                                canvas[(by + bh - 1) * term_width + x].bg = box_clr;
                }
                for (int y = by; y < by + bh; y++)
                {
                        if (bx >= 0 && bx < term_width && y >= s->p.y && y < s->p.y + s->p.h)
                                canvas[y * term_width + bx].bg = box_clr;
                        if (bx + bw - 1 >= 0 && bx + bw - 1 < term_width && y >= s->p.y && y < s->p.y + s->p.h)
                                canvas[y * term_width + bx + bw - 1].bg = box_clr;
                }

                if (!term_mouse.left)
                {
                        for (int i = 0; i < s->p.item_count; i++)
                                if (s->active_box_selections[i])
                                        s->selections[i] = true;
                        s->is_box_selecting = false;
                }
        }

        if (max_scroll > 0)
        {
                bool hover = (!s->dragging_scroll && term_mouse.x == s->p.x + s->p.w - 1 && term_mouse.y >= s->p.y && term_mouse.y < s->p.y + s->p.h);
                Color thumb_col = s->dragging_scroll ? (Color){255, 255, 255} : s->p.scrollbar_fg;
                int thumb_h_half = (s->p.h * 2) * s->p.h / (rows * c_h);
                if (thumb_h_half < 2)
                        thumb_h_half = 2;
                int thumb_top_half = (int)((s->current_scroll / max_scroll) * (s->p.h * 2 - thumb_h_half));
                int thumb_bot_half = thumb_top_half + thumb_h_half;

                for (int y = 0; y < s->p.h; y++)
                {
                        int cell_top = y * 2, cell_bot = y * 2 + 1;
                        bool top_in = (cell_top >= thumb_top_half && cell_top < thumb_bot_half);
                        bool bot_in = (cell_bot >= thumb_top_half && cell_bot < thumb_bot_half);

                        const char *ch = (top_in && bot_in) ? " " : top_in ? "\xe2\x96\x80"
                                                                : bot_in   ? "\xe2\x96\x84"
                                                                : hover    ? "\xe2\x96\x91"
                                                                           : " ";
                        Color fg = (hover && !top_in && !bot_in) ? s->p.scrollbar_fg : thumb_col;
                        Color bg = (top_in && bot_in) ? thumb_col : s->p.scrollbar_bg;
                        ui_text(s->p.x + s->p.w - 1, s->p.y + y, ch, fg, bg, false, false);
                }
        }

        if (s->drag_idx != -1)
        {
                if (term_mouse.left)
                {
                        if (!s->is_dragging && (ui_fabsf(term_mouse.x - s->drag_start_x) > 0 || ui_fabsf(term_mouse.y - s->drag_start_y) > 0))
                        {
                                s->is_dragging = true;
                                s->pickup_anim = 1.0f;
                                s->carry_x = term_mouse.x - (s->mode == UI_MODE_LIST ? 2 : s->drag_off_x);
                                s->carry_y = term_mouse.y - (s->mode == UI_MODE_LIST ? 1 : s->drag_off_y);
                        }
                }
                else
                {
                        if (s->is_dragging)
                        {
                                s->action_drop_src = s->drag_idx;
                                s->action_drop_dst = s->drop_target_idx;
                        }
                        else
                                s->action_click_idx = s->drag_idx;
                        s->drag_idx = s->drop_target_idx = -1;
                        s->is_dragging = false;
                }
        }
}

bool ui_list_is_animating(UIListState *s)
{
        if (s->dragging_scroll || s->is_dragging || s->is_kb_dragging || s->is_box_selecting || s->carrying)
                return true;
        if (s->drop_anim > 0.0f || s->fly_anim > 0.0f || s->pickup_anim > 0.0f)
                return true;

        float diff = s->target_scroll - s->current_scroll;
        return (diff > 0.01f || diff < -0.01f || s->scroll_velocity > 0.01f || s->scroll_velocity < -0.01f);
}

void ui_list_tick_animations(UIListState *s, int sel_x, int sel_y)
{
        if (s->carrying)
        {
                float tx = sel_x + (s->mode == UI_MODE_LIST ? 45 : s->p.cell_w / 2);
                float ty = sel_y + (s->mode == UI_MODE_LIST ? -1 : s->p.cell_h / 2);
                s->carry_x += (tx - s->carry_x) * ANIM_SPEED_CARRY;
                s->carry_y += (ty - s->carry_y) * ANIM_SPEED_CARRY;
        }
        else if (s->is_dragging)
        {
                s->carry_x = term_mouse.x - (s->mode == UI_MODE_LIST ? 2 : s->drag_off_x);
                s->carry_y = term_mouse.y - (s->mode == UI_MODE_LIST ? 1 : s->drag_off_y);
        }
        else if (s->is_kb_dragging)
        {
                s->carry_x = s->kb_drag_x;
                s->carry_y = s->kb_drag_y;
        }

        if (s->fly_anim > 0.0f)
                s->fly_anim = (s->fly_anim - ANIM_SPEED_FLY * term_dt_scale < 0.0f) ? 0.0f : s->fly_anim - ANIM_SPEED_FLY * term_dt_scale;
        if (s->pickup_anim > 0.0f)
                s->pickup_anim = (s->pickup_anim - ANIM_SPEED_FLY * term_dt_scale < 0.0f) ? 0.0f : s->pickup_anim - ANIM_SPEED_FLY * term_dt_scale;
        if (s->drop_anim > 0.0f)
                s->drop_anim = (s->drop_anim - ANIM_SPEED_DROP * term_dt_scale < 0.0f) ? 0.0f : s->drop_anim - ANIM_SPEED_DROP * term_dt_scale;
}

bool ui_list_get_anim_coords(UIListState *s, int base_x, int base_y, bool is_dropped, bool is_picked_up, int *out_x, int *out_y)
{
        if (is_dropped || (is_picked_up && s->pickup_anim > 0.01f))
        {
                float e = is_dropped ? s->drop_anim * s->drop_anim : s->pickup_anim * s->pickup_anim;
                *out_x = is_dropped ? (s->drop_to_target ? s->drop_dst_x + (s->carry_x - s->drop_dst_x) * e : base_x + (s->carry_x - base_x) * e) : s->carry_x + (base_x - s->carry_x) * e;
                *out_y = is_dropped ? (s->drop_to_target ? s->drop_dst_y + (s->carry_y - s->drop_dst_y) * e : base_y + (s->carry_y - base_y) * e) : s->carry_y + (base_y - s->carry_y) * e;
                return true;
        }
        return false;
}

bool ui_list_get_fly_coords(UIListState *s, int *out_x, int *out_y)
{
        if (s->fly_anim > 0.0f)
        {
                float ease = s->fly_anim * s->fly_anim;
                *out_x = s->fly_is_pickup ? s->carry_x + (s->fly_origin_x - s->carry_x) * ease : s->fly_origin_x + (s->carry_x - s->fly_origin_x) * ease;
                *out_y = s->fly_is_pickup ? s->carry_y + (s->fly_origin_y - s->carry_y) * ease : s->fly_origin_y + (s->carry_y - s->fly_origin_y) * ease;
                return true;
        }
        return false;
}

void ui_context_open(int target_idx)
{
        global_ctx = (UIContextState){.active = true, .x = term_mouse.x, .y = term_mouse.y, .w = 0};
        global_ctx.list.selected_idx = -1;
        global_ctx.list.mode = UI_MODE_LIST;
        global_ctx_target = target_idx;
        term_mouse.right_clicked = false;
}

int ui_context_target(void) { return global_ctx_target; }

void ui_context_close(void)
{
        global_ctx.active = false;
        global_ctx.w = 0;
}

void ui_draw_badge(int x, int y, int count)
{
        if (count <= 1)
                return;
        raw char badge[16];
        snprintf(badge, sizeof(badge), " %d ", count);
        ui_text(x, y, badge, (Color){255, 255, 255}, (Color){200, 50, 50}, true, false);
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
                        global_ctx.x = (term_width - global_ctx.w < 0) ? 0 : term_width - global_ctx.w;
                if (global_ctx.y + global_ctx.h > term_height)
                {
                        global_ctx.h = term_height - global_ctx.y;
                        if (global_ctx.h < 4 && count >= 4)
                        {
                                global_ctx.y = (term_height - count < 0) ? 0 : term_height - count;
                                global_ctx.h = (count > term_height) ? term_height : count;
                        }
                }
        }

        ui_rect(global_ctx.x, global_ctx.y, global_ctx.w, global_ctx.h, (Color){15, 15, 15});
        UIListParams params = {.x = global_ctx.x, .y = global_ctx.y, .w = global_ctx.w, .h = global_ctx.h, .item_count = count, .cell_w = global_ctx.w, .cell_h = 1, .bg = (Color){15, 15, 15}, .scrollbar_bg = (Color){25, 25, 25}, .scrollbar_fg = (Color){100, 100, 100}};

        ui_list_begin(&global_ctx.list, &params, 0);
        defer ui_list_end(&global_ctx.list); // Magic cleanup!

        bool action_taken = false;

        for (int i = 0; i < count; i++)
        {
                UIItemResult item;
                (ui_list_do_item(&global_ctx.list, i, &item)) orelse continue;

                Color bg = item.pressed ? (Color){60, 60, 60} : (item.hovered || global_ctx.list.selected_idx == i) ? (Color){35, 35, 35}
                                                                                                                    : (Color){15, 15, 15};
                ui_rect(item.x, item.y, item.w, item.h, bg);
                ui_text(item.x + 1, item.y, items[i], (Color){255, 255, 255}, bg, false, false);

                if (item.clicked)
                {
                        *out_idx = i;
                        action_taken = true;
                }
        }

        if ((term_mouse.clicked || term_mouse.right_clicked) && !action_taken && !global_ctx.list.dragging_scroll)
                if (!(term_mouse.x >= global_ctx.x && term_mouse.x < global_ctx.x + params.w && term_mouse.y >= global_ctx.y && term_mouse.y < global_ctx.y + params.h))
                {
                        global_ctx.active = false;
                        global_ctx.w = 0;
                }

        if (action_taken)
        {
                global_ctx.active = false;
                global_ctx.w = 0;
        }
        return action_taken;
}

bool ui_text_input(int x, int y, int w, char *buf, size_t buf_size, int key, bool active)
{
        if (active)
        {
                int len = strlen(buf);
                if (key == KEY_BACKSPACE && len > 0)
                        buf[len - 1] = '\0';
                else if (key >= 32 && key <= 126 && len < buf_size - 1)
                {
                        buf[len] = (char)key;
                        buf[len + 1] = '\0';
                }
        }

        Color bg = active ? (Color){45, 45, 45} : (Color){20, 20, 20};
        ui_rect(x, y, w, 1, bg);
        raw char disp[256];
        snprintf(disp, sizeof(disp), "%s%s", buf, active ? "_" : "");
        ui_text(x + 1, y, disp, (Color){255, 255, 255}, bg, false, false);

        return (active && key == KEY_ENTER);
}