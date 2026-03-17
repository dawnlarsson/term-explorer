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
#define KEY_CTRL_BACKSPACE 1011
#define KEY_F1 1012

#define KEY_SHIFT_PAGE_DOWN 1009
#define KEY_DELETE 1010
#define KEY_ENTER 10

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
        int x, y, w, h;
} View;

typedef struct
{
        int x, y, sub_y, wheel;
        bool has_sub, left, right, clicked, right_clicked, hide_cursor, ctrl;
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

        bool fly_is_pickup, drop_to_target, carrying, external_drag;
} UIListState;

typedef struct
{
        bool active;
        void *id;
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

typedef struct
{
        View first, second;
        int divider_x;
} UISplitterLayout;

#define UI_DOCK_MAX_TABS 64
#define UI_DOCK_MAX_NODES 32

typedef enum
{
        UI_DOCK_NODE_LEAF,
        UI_DOCK_NODE_SPLIT_H,
} UIDockNodeKind;

typedef enum
{
        UI_DOCK_DROP_NONE,
        UI_DOCK_DROP_MERGE,
        UI_DOCK_DROP_SPLIT_LEFT,
        UI_DOCK_DROP_SPLIT_RIGHT,
} UIDockDropKind;

typedef struct
{
        bool in_use;
        UIDockNodeKind kind;
        int parent;
        View view;
        int tabs[UI_DOCK_MAX_TABS];
        int tab_count;
        int active_tab;
        int first, second;
        float split_frac;
        bool dragging_splitter;
} UIDockNode;

typedef struct
{
        UIDockNode nodes[UI_DOCK_MAX_NODES];
        int root;
        View bounds;
        int active_leaf;
        int last_active_leaf;
        float switch_flash;
        int dragging_tab;
        int drag_src_leaf;
        int drag_start_x;
        int pending_tab;
        int pending_leaf;
        int pending_start_x;
        int pending_frames;
        int close_request_tab;
        int add_request_leaf;
} UIDockState;

typedef struct
{
        const char *label;
        const char *title; /* The full path/info to show in the bar */
        bool active, closable;
} UITab;

typedef struct
{
        int clicked_tab;
        int close_tab;
        bool add_clicked;
} UITabBarResult;

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
static View *active_view = NULL;
static const char *current_cursor = "text";
static const char *next_cursor = "default";
static bool mouse_suppressed = false;
static Cell *canvas, *last_canvas;
static int fd_m = -1, fd_touch = -1, raw_mx, raw_my, color_mode;
static bool is_evdev;
static int touch_min_x, touch_max_x, touch_min_y, touch_max_y;
static char out_buf[1024 * 1024];
static UIContextState global_ctx;
static int global_ctx_target = -1;

static bool ui_mouse_in_active_view(void)
{
        if (!active_view)
                return true;
        int x = term_mouse.x - active_view->x;
        int y = term_mouse.y - active_view->y;
        return x >= 0 && x < active_view->w && y >= 0 && y < active_view->h;
}

static int ui_list_cols(const UIListState *s)
{
        return (s->mode == UI_MODE_LIST) ? 1 : ((s->p.w - 1) / s->p.cell_w > 0 ? (s->p.w - 1) / s->p.cell_w : 1);
}

UIRect ui_list_item_rect(const UIListState *s, int index)
{
        int cols = ui_list_cols(s);
        int c_w = (s->mode == UI_MODE_LIST) ? s->p.w - 1 : s->p.cell_w;
        int c_h = (s->mode == UI_MODE_LIST) ? 1 : s->p.cell_h;

        int col_idx = index % cols;
        int row_idx = index / cols;

        int x_pos = s->p.x;
        if (s->mode != UI_MODE_LIST)
        {
                int extra = (s->p.w - 1) - cols * c_w;
                if (extra < 0)
                        extra = 0;
                int dynamic_off_x = ((col_idx + 1) * extra) / (cols + 1);
                x_pos = s->p.x + dynamic_off_x + col_idx * c_w;
        }

        return (UIRect){
            .x = x_pos,
            .y = s->p.y + (row_idx * c_h) - (int)(s->current_scroll + 0.5f),
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

#define SET_COL(fg, c) (c.r == -1 ? printf("[%d9m", fg ? 3 : 4) : (color_mode == 2 ? printf("[%d;2;%d;%d;%dm", fg ? 38 : 48, c.r, c.g, c.b) : printf("[%d;5;%dm", fg ? 38 : 48, 16 + (36 * (c.r / 51)) + (6 * (c.g / 51)) + (c.b / 51))))

void term_restore(void)
{
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\033]22;text\007"); /* Restore the default terminal text cursor */
        printf("\033%%@\033[0m\033[2J\033[H\033[?25h\033[?7h\033[?1006l\033[?1015l\033[?1003l");
        fflush(stdout);
        if (fd_m >= 0)
                close(fd_m);
        if (fd_touch >= 0)
                close(fd_touch);
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

        for (int i = 0; i < 32 && fd_touch < 0; i++)
        {
                raw char path[64];
                snprintf(path, sizeof(path), "/dev/input/event%d", i);
                int fd = open(path, O_RDONLY | O_NONBLOCK);
                (fd >= 0) orelse continue;

                raw unsigned long ev[EV_MAX / 8 + 1];
                raw unsigned long abs_bits[ABS_MAX / 8 + 1];
                memset(ev, 0, sizeof(ev));
                memset(abs_bits, 0, sizeof(abs_bits));
                ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
                ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);

                bool has_mt_x = (abs_bits[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8))) != 0;
                bool has_mt_y = (abs_bits[ABS_MT_POSITION_Y / 8] & (1 << (ABS_MT_POSITION_Y % 8))) != 0;
                bool has_abs_x = (abs_bits[ABS_X / 8] & (1 << (ABS_X % 8))) != 0;
                bool has_abs_y = (abs_bits[ABS_Y / 8] & (1 << (ABS_Y % 8))) != 0;

                if ((has_mt_x && has_mt_y) || (has_abs_x && has_abs_y))
                {
                        raw struct input_absinfo ax, ay;
                        int code_x = has_mt_x ? ABS_MT_POSITION_X : ABS_X;
                        int code_y = has_mt_y ? ABS_MT_POSITION_Y : ABS_Y;
                        ioctl(fd, EVIOCGABS(code_x), &ax);
                        ioctl(fd, EVIOCGABS(code_y), &ay);
                        fd_touch = fd;
                        touch_min_x = ax.minimum;
                        touch_max_x = ax.maximum;
                        touch_min_y = ay.minimum;
                        touch_max_y = ay.maximum;
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
        raw struct pollfd fds[3];
        int nfds = 0;
        fds[nfds++] = (struct pollfd){STDIN_FILENO, POLLIN, 0};
        if (fd_m >= 0)
                fds[nfds++] = (struct pollfd){fd_m, POLLIN, 0};
        if (fd_touch >= 0)
                fds[nfds++] = (struct pollfd){fd_touch, POLLIN, 0};
        poll(fds, nfds, timeout_ms);

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
                term_mouse = (Mouse){term_width / 2, term_height / 2, 0, 0, false, false, false, false, false, true, false};
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

#ifdef __linux__
        if (fd_touch >= 0)
        {
                raw struct input_event ev;
                while (read(fd_touch, &ev, sizeof(ev)) == sizeof(ev))
                {
                        term_mouse.hide_cursor = false;
                        if (ev.type == EV_ABS)
                        {
                                if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X)
                                {
                                        int range = touch_max_x - touch_min_x;
                                        if (range > 0)
                                                raw_mx = (ev.value - touch_min_x) * (term_width * 8) / range;
                                }
                                if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y)
                                {
                                        int range = touch_max_y - touch_min_y;
                                        if (range > 0)
                                                raw_my = (ev.value - touch_min_y) * (term_height * 16) / range;
                                }
                        }
                        if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
                                term_mouse.left = ev.value;
                }
                raw_mx = raw_mx < 0 ? 0 : (raw_mx >= term_width * 8 ? term_width * 8 - 1 : raw_mx);
                raw_my = raw_my < 0 ? 0 : (raw_my >= term_height * 16 ? term_height * 16 - 1 : raw_my);
                term_mouse.x = raw_mx / 8;
                term_mouse.y = raw_my / 16;
                term_mouse.sub_y = (raw_my / 8) % 2;
                term_mouse.has_sub = true;
        }
#endif

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
                                        bool d = (m == 'M');
                                        bool ctrl = (b & 16) != 0;
                                        int btn = b & ~28;

                                        term_mouse = (Mouse){x - 1, y - 1, 0, term_mouse.wheel, false, term_mouse.left, term_mouse.right, false, false, true, ctrl};

                                        if (m == 'm' || btn == 3 || btn == 35)
                                                term_mouse.left = term_mouse.right = false;
                                        else if (btn == 0 || btn == 32)
                                                term_mouse.left = d;
                                        else if (btn == 2 || btn == 34)
                                                term_mouse.right = d;
                                        if (btn == 64 && d)
                                                term_mouse.wheel--;
                                        if (btn == 65 && d)
                                                term_mouse.wheel++;
                                        i += 3 + offset;
                                        continue;
                                }
                                else if (i + 5 < n && buf[i + 5] == '~' && buf[i + 3] == ';' && buf[i + 4] == '5')
                                {
                                        key = (buf[i + 2] == '3') ? KEY_CTRL_BACKSPACE : 0;
                                        if (key)
                                        {
                                                i += 5;
                                                continue;
                                        }
                                }
                                else if (i + 4 < n && buf[i + 4] == '~' && buf[i + 2] == '1' && buf[i + 3] == '1')
                                {
                                        key = KEY_F1;
                                        i += 4;
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
                                                                     : (c == '3')             ? KEY_DELETE
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
                                                                          : (buf[i + 2] == 'P')   ? KEY_F1
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
                                key = (buf[i] == '\r') ? KEY_ENTER : (buf[i] == 127) ? KEY_BACKSPACE
                                                                 : (buf[i] == 8)     ? KEY_CTRL_BACKSPACE
                                                                                     : buf[i];
                }
        }

        term_mouse.clicked = (!last_left && term_mouse.left);
        term_mouse.right_clicked = (!last_right && term_mouse.right);
        return key;
}

void ui_begin(void)
{
        next_cursor = "default";
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

void ui_set_view(View *v) { active_view = v; }
void ui_set_cursor(const char *cursor) { next_cursor = cursor; }
void ui_suppress_mouse(bool suppress) { mouse_suppressed = suppress; }
void ui_rect(int x, int y, int w, int h, Color bg);
Mouse ui_get_mouse(void)
{
        Mouse m = term_mouse;
        if (mouse_suppressed)
        {
                m.left = false;
                m.right = false;
                m.clicked = false;
                m.right_clicked = false;
                m.wheel = 0;
                m.has_sub = false;
        }

        if (active_view)
        {
                m.x -= active_view->x;
                m.y -= active_view->y;
        }
        return m;
}

void ui_clear(Color bg)
{
        if (active_view)
        {
                ui_rect(0, 0, active_view->w, active_view->h, bg);
                return;
        }
        for (int i = 0; i < term_width * term_height; i++)
                canvas[i] = (Cell){" ", bg, bg, false, false};
}

void ui_rect(int x, int y, int w, int h, Color bg)
{
        if (active_view)
        {
                x += active_view->x;
                y += active_view->y;
        }
        for (int r = y; r < y + h; r++)
        {
                if (r < 0 || r >= term_height || (active_view && (r < active_view->y || r >= active_view->y + active_view->h)))
                        continue;
                for (int c = x; c < x + w; c++)
                {
                        if (c < 0 || c >= term_width || (active_view && (c < active_view->x || c >= active_view->x + active_view->w)))
                                continue;
                        canvas[r * term_width + c] = (Cell){" ", bg, bg, false, false};
                }
        }
}

void ui_text(int x, int y, const char *txt, Color fg, Color bg, bool bold, bool invert)
{
        if (active_view)
        {
                x += active_view->x;
                y += active_view->y;
        }
        if (y < 0 || y >= term_height)
                return;
        int x_min = active_view ? active_view->x : 0;
        int x_max = active_view ? active_view->x + active_view->w : term_width;
        for (int i = 0, sx = x; txt[i] && sx < x_max; sx++)
        {
                int len = ((txt[i] & 0xE0) == 0xC0) ? 2 : ((txt[i] & 0xF0) == 0xE0) ? 3
                                                      : ((txt[i] & 0xF8) == 0xF0)   ? 4
                                                                                    : 1;
                int actual_len = 0;
                if (sx >= x_min && sx < term_width)
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

UITabBarResult ui_tab_bar(int w, const UITab *tabs, int count, bool show_add, bool is_active_pane, float switch_flash)
{
        UITabBarResult res = {.clicked_tab = -1, .close_tab = -1, .add_clicked = false};
        Color bar_bg = (Color){0, 0, 0};

        Color active_bg = is_active_pane ? (Color){255, 255, 255} : (Color){150, 150, 150};
        Color active_fg = (Color){0, 0, 0};

        if (is_active_pane && switch_flash > 0.0f)
        {
                float t = switch_flash;
                active_bg = (Color){255 * (1 - t), 255 * (1 - t), 255 * (1 - t)};
                active_fg = (Color){255 * t, 255 * t, 255 * t};
        }

        Color inactive_bg = is_active_pane ? (Color){150, 150, 150} : (Color){80, 80, 80};
        Color inactive_fg = (Color){50, 50, 50};
        Color close_fg = (Color){180, 80, 80};
        Color add_fg = (Color){100, 200, 100};

        ui_rect(0, 0, w, 1, bar_bg);

        Mouse m = ui_get_mouse();
        int tx = 0;
        const char *active_title = NULL;

        for (int i = 0; i < count; i++)
        {
                if (tabs[i].active && tabs[i].title)
                        active_title = tabs[i].title;

                const char *label = tabs[i].label ? tabs[i].label : "";
                int name_len = 0;
                for (const char *c = label; *c; c++)
                        name_len++;
                int tab_w = name_len + 4;
                if (tab_w < 8)
                        tab_w = 8;
                if (tx + tab_w > w)
                        tab_w = w - tx;
                if (tab_w <= 0)
                        break;

                Color tab_bg = tabs[i].active ? active_bg : inactive_bg;
                Color tab_fg = tabs[i].active ? active_fg : inactive_fg;

                ui_rect(tx, 0, tab_w, 1, tab_bg);
                ui_text(tx + 1, 0, label, tab_fg, tab_bg, tabs[i].active, false);
                if (tabs[i].closable)
                        ui_text(tx + tab_w - 2, 0, "×", close_fg, tab_bg, false, false);

                if (m.y == 0 && m.x >= tx && m.x < tx + tab_w)
                {
                        ui_set_cursor("pointer");
                        if (m.clicked)
                        {
                                if (tabs[i].closable && m.x == tx + tab_w - 2)
                                        res.close_tab = i;
                                else
                                        res.clicked_tab = i;
                        }
                }

                tx += tab_w + 1;
        }

        if (show_add && tx + 3 <= w)
        {
                if (m.y == 0 && m.x >= tx && m.x < tx + 3)
                        ui_set_cursor("pointer");
                ui_text(tx, 0, " + ", add_fg, bar_bg, false, false);
                if (m.clicked && m.y == 0 && m.x >= tx && m.x < tx + 3)
                        res.add_clicked = true;
                tx += 4;
        }

        if (active_title && tx < w)
        {
                Color title_fg = is_active_pane ? (Color){255, 255, 255} : (Color){120, 120, 120};
                ui_text(tx + 1, 0, active_title, title_fg, bar_bg, false, false);
        }

        return res;
}

bool ui_view_contains(const View *view, int x, int y)
{
        return view && x >= view->x && x < view->x + view->w && y >= view->y && y < view->y + view->h;
}

UISplitterLayout ui_splitter_h(float *frac, bool *dragging, int x, int y, int w, int h, bool split, float min_frac, float max_frac)
{
        UISplitterLayout layout = {
            .first = {x, y, w, h},
            .second = {x, y, 0, h},
            .divider_x = x + w};

        if (!split)
                return layout;

        int divider_x = x + (int)(w * *frac);
        bool hovering = term_mouse.x == divider_x && term_mouse.y >= y && term_mouse.y < y + h;
        if (hovering)
                ui_set_cursor("ew-resize");

        if (term_mouse.clicked && hovering)
                *dragging = true;
        if (!term_mouse.left)
                *dragging = false;

        if (*dragging)
        {
                ui_set_cursor("ew-resize");
                float f = (float)(term_mouse.x - x) / w;
                if (f < min_frac)
                        f = min_frac;
                if (f > max_frac)
                        f = max_frac;
                *frac = f;
                divider_x = x + (int)(w * *frac);
        }

        layout.first = (View){x, y, divider_x - x, h};
        layout.second = (View){divider_x + 1, y, x + w - divider_x - 1, h};
        layout.divider_x = divider_x;

        return layout;
}

void ui_splitter_h_draw(const UISplitterLayout *layout, bool dragging, int y, int h, Color divider_fg, Color bg)
{
        if (!layout || layout->second.w <= 0)
                return;
        bool hover = (term_mouse.x == layout->divider_x && term_mouse.y >= y && term_mouse.y < y + h);
        Color div = (dragging || hover) ? (Color){255, 255, 255} : divider_fg;
        for (int row = y; row < y + h; row++)
                ui_text(layout->divider_x, row, "│", div, bg, false, false);
}

static void ui_dock_reset_node(UIDockNode *node)
{
        memset(node, 0, sizeof(*node));
        node->in_use = true;
        node->kind = UI_DOCK_NODE_LEAF;
        node->parent = -1;
        node->active_tab = -1;
        node->first = -1;
        node->second = -1;
        node->split_frac = 0.5f;
}

static void ui_dock_release_node(UIDockNode *node)
{
        memset(node, 0, sizeof(*node));
        node->parent = -1;
        node->active_tab = -1;
        node->first = -1;
        node->second = -1;
}

static int ui_dock_alloc_node(UIDockState *dock)
{
        for (int i = 0; i < UI_DOCK_MAX_NODES; i++)
                if (!dock->nodes[i].in_use)
                {
                        ui_dock_reset_node(&dock->nodes[i]);
                        return i;
                }
        return -1;
}

static int ui_dock_leaf_first_valid_tab(const UIDockNode *leaf)
{
        return leaf->tab_count > 0 ? leaf->tabs[0] : -1;
}

static int ui_dock_leaf_find_tab(const UIDockNode *leaf, int tab_id)
{
        for (int i = 0; i < leaf->tab_count; i++)
                if (leaf->tabs[i] == tab_id)
                        return i;
        return -1;
}

static bool ui_dock_is_leaf_node(const UIDockState *dock, int node_idx)
{
        return dock && node_idx >= 0 && node_idx < UI_DOCK_MAX_NODES && dock->nodes[node_idx].in_use && dock->nodes[node_idx].kind == UI_DOCK_NODE_LEAF;
}

static int ui_dock_first_leaf_from(const UIDockState *dock, int node_idx)
{
        if (!dock || node_idx < 0 || node_idx >= UI_DOCK_MAX_NODES)
                return -1;
        const UIDockNode *node = &dock->nodes[node_idx];
        if (!node->in_use)
                return -1;
        if (node->kind == UI_DOCK_NODE_LEAF)
                return node_idx;
        int leaf = ui_dock_first_leaf_from(dock, node->first);
        return leaf >= 0 ? leaf : ui_dock_first_leaf_from(dock, node->second);
}

static void ui_dock_replace_child(UIDockState *dock, int parent_idx, int old_child, int new_child)
{
        if (parent_idx < 0)
        {
                dock->root = new_child;
                if (new_child >= 0)
                        dock->nodes[new_child].parent = -1;
                return;
        }

        UIDockNode *parent = &dock->nodes[parent_idx];
        if (parent->first == old_child)
                parent->first = new_child;
        else if (parent->second == old_child)
                parent->second = new_child;

        if (new_child >= 0)
                dock->nodes[new_child].parent = parent_idx;
}

static void ui_dock_collapse_empty_leaf(UIDockState *dock, int leaf_idx)
{
        if (!ui_dock_is_leaf_node(dock, leaf_idx))
                return;

        UIDockNode *leaf = &dock->nodes[leaf_idx];
        if (leaf->tab_count > 0)
                return;

        int parent_idx = leaf->parent;
        if (parent_idx < 0)
        {
                leaf->active_tab = -1;
                dock->active_leaf = leaf_idx;
                return;
        }

        UIDockNode *parent = &dock->nodes[parent_idx];
        int sibling_idx = parent->first == leaf_idx ? parent->second : parent->first;
        int grand_idx = parent->parent;

        ui_dock_replace_child(dock, grand_idx, parent_idx, sibling_idx);
        if (dock->active_leaf == leaf_idx)
                dock->active_leaf = ui_dock_first_leaf_from(dock, sibling_idx);

        ui_dock_release_node(leaf);
        ui_dock_release_node(parent);
}

static bool ui_dock_remove_tab_from_leaf(UIDockState *dock, int leaf_idx, int tab_id)
{
        if (!ui_dock_is_leaf_node(dock, leaf_idx))
                return false;

        UIDockNode *leaf = &dock->nodes[leaf_idx];
        int idx = ui_dock_leaf_find_tab(leaf, tab_id);
        if (idx < 0)
                return false;

        memmove(&leaf->tabs[idx], &leaf->tabs[idx + 1], (leaf->tab_count - idx - 1) * sizeof(int));
        leaf->tab_count--;
        if (leaf->active_tab == tab_id)
                leaf->active_tab = ui_dock_leaf_first_valid_tab(leaf);
        if (leaf->tab_count == 0)
                ui_dock_collapse_empty_leaf(dock, leaf_idx);
        return true;
}

static void ui_dock_collect_leaves(const UIDockState *dock, int node_idx, int *out, int *count, int max_count)
{
        if (!dock || node_idx < 0 || node_idx >= UI_DOCK_MAX_NODES || *count >= max_count)
                return;
        const UIDockNode *node = &dock->nodes[node_idx];
        if (!node->in_use)
                return;
        if (node->kind == UI_DOCK_NODE_LEAF)
        {
                out[(*count)++] = node_idx;
                return;
        }
        ui_dock_collect_leaves(dock, node->first, out, count, max_count);
        ui_dock_collect_leaves(dock, node->second, out, count, max_count);
}

int ui_dock_leaf_at_point(const UIDockState *dock, int node_idx, int x, int y)
{
        if (!dock || node_idx < 0 || node_idx >= UI_DOCK_MAX_NODES)
                return -1;

        const UIDockNode *node = &dock->nodes[node_idx];
        if (!node->in_use || !ui_view_contains(&node->view, x, y))
                return -1;
        if (node->kind == UI_DOCK_NODE_LEAF)
                return node_idx;

        int hit = ui_dock_leaf_at_point(dock, node->first, x, y);
        return hit >= 0 ? hit : ui_dock_leaf_at_point(dock, node->second, x, y);
}

static void ui_dock_layout_node(UIDockState *dock, int node_idx, View view)
{
        if (!dock || node_idx < 0 || node_idx >= UI_DOCK_MAX_NODES)
                return;

        UIDockNode *node = &dock->nodes[node_idx];
        if (!node->in_use)
                return;
        node->view = view;
        if (node->kind == UI_DOCK_NODE_LEAF)
                return;

        float min_f = 2.0f / (float)(view.w > 0 ? view.w : 1);
        if (min_f > 0.4f)
                min_f = 0.4f;

        UISplitterLayout split = ui_splitter_h(&node->split_frac, &node->dragging_splitter, view.x, view.y, view.w, view.h, true, min_f, 1.0f - min_f);
        ui_dock_layout_node(dock, node->first, split.first);
        ui_dock_layout_node(dock, node->second, split.second);
}

static void ui_dock_draw_splitters(const UIDockState *dock, int node_idx, Color divider_fg, Color bg)
{
        if (!dock || node_idx < 0 || node_idx >= UI_DOCK_MAX_NODES)
                return;

        const UIDockNode *node = &dock->nodes[node_idx];
        if (!node->in_use || node->kind == UI_DOCK_NODE_LEAF)
                return;

        int divider_x = dock->nodes[node->second].view.x - 1;
        UISplitterLayout layout = {
            .first = dock->nodes[node->first].view,
            .second = dock->nodes[node->second].view,
            .divider_x = divider_x,
        };
        ui_splitter_h_draw(&layout, node->dragging_splitter, node->view.y, node->view.h, divider_fg, bg);
        ui_dock_draw_splitters(dock, node->first, divider_fg, bg);
        ui_dock_draw_splitters(dock, node->second, divider_fg, bg);
}

static UIDockDropKind ui_dock_drop_kind_for_leaf(const UIDockState *dock, int target_leaf, int src_leaf, int mouse_x)
{
        if (!ui_dock_is_leaf_node(dock, target_leaf))
                return UI_DOCK_DROP_NONE;

        const UIDockNode *leaf = &dock->nodes[target_leaf];
        if (target_leaf == src_leaf && leaf->tab_count <= 1)
                return UI_DOCK_DROP_NONE;

        int left_edge = leaf->view.x + leaf->view.w / 3;
        int right_edge = leaf->view.x + (leaf->view.w * 2) / 3;
        if (mouse_x < left_edge)
                return UI_DOCK_DROP_SPLIT_LEFT;
        if (mouse_x >= right_edge)
                return UI_DOCK_DROP_SPLIT_RIGHT;
        return target_leaf != src_leaf ? UI_DOCK_DROP_MERGE : UI_DOCK_DROP_NONE;
}

static View ui_dock_preview_rect_for_drop(const UIDockState *dock, int target_leaf, UIDockDropKind kind)
{
        View empty = {0, 0, 0, 0};
        if (!ui_dock_is_leaf_node(dock, target_leaf))
                return empty;

        View view = dock->nodes[target_leaf].view;
        if (kind == UI_DOCK_DROP_MERGE)
                return view;
        if (kind == UI_DOCK_DROP_SPLIT_LEFT)
                return (View){view.x, view.y, view.w / 2, view.h};
        if (kind == UI_DOCK_DROP_SPLIT_RIGHT)
                return (View){view.x + view.w / 2, view.y, view.w - view.w / 2, view.h};
        return empty;
}

int ui_dock_leaf_count(const UIDockState *dock)
{
        int leaves[UI_DOCK_MAX_NODES];
        int count = 0;
        memset(leaves, 0, sizeof(leaves));
        if (dock)
                ui_dock_collect_leaves(dock, dock->root, leaves, &count, UI_DOCK_MAX_NODES);
        return count;
}

int ui_dock_leaf_nth(const UIDockState *dock, int n)
{
        int leaves[UI_DOCK_MAX_NODES];
        int count = 0;
        memset(leaves, 0, sizeof(leaves));
        if (!dock)
                return -1;
        ui_dock_collect_leaves(dock, dock->root, leaves, &count, UI_DOCK_MAX_NODES);
        return n >= 0 && n < count ? leaves[n] : -1;
}

void ui_dock_init(UIDockState *dock)
{
        memset(dock, 0, sizeof(*dock));
        dock->root = 0;
        ui_dock_reset_node(&dock->nodes[0]);
        dock->bounds = (View){0, 0, 0, 0};
        dock->dragging_tab = -1;
        dock->drag_src_leaf = -1;
        dock->pending_tab = -1;
        dock->pending_leaf = -1;
        dock->close_request_tab = -1;
        dock->add_request_leaf = -1;
        dock->active_leaf = 0;
}

bool ui_dock_add_tab_to_leaf(UIDockState *dock, int leaf_idx, int tab_id)
{
        if (!ui_dock_is_leaf_node(dock, leaf_idx))
                return false;
        UIDockNode *leaf = &dock->nodes[leaf_idx];
        if (leaf->tab_count >= UI_DOCK_MAX_TABS)
                return false;
        leaf->tabs[leaf->tab_count++] = tab_id;
        leaf->active_tab = tab_id;
        dock->active_leaf = leaf_idx;
        return true;
}

int ui_dock_split_leaf(UIDockState *dock, int leaf_idx, bool new_leaf_before)
{
        if (!ui_dock_is_leaf_node(dock, leaf_idx))
                return -1;

        int new_leaf_idx = ui_dock_alloc_node(dock);
        if (new_leaf_idx < 0)
                return -1;
        int parent_idx = ui_dock_alloc_node(dock);
        if (parent_idx < 0)
        {
                ui_dock_release_node(&dock->nodes[new_leaf_idx]);
                return -1;
        }

        UIDockNode *leaf = &dock->nodes[leaf_idx];
        int old_parent = leaf->parent;

        UIDockNode *parent = &dock->nodes[parent_idx];
        parent->kind = UI_DOCK_NODE_SPLIT_H;
        parent->parent = old_parent;
        parent->view = leaf->view;
        parent->split_frac = 0.5f;
        parent->dragging_splitter = false;

        dock->nodes[new_leaf_idx].parent = parent_idx;
        leaf->parent = parent_idx;

        if (new_leaf_before)
        {
                parent->first = new_leaf_idx;
                parent->second = leaf_idx;
        }
        else
        {
                parent->first = leaf_idx;
                parent->second = new_leaf_idx;
        }

        ui_dock_replace_child(dock, old_parent, leaf_idx, parent_idx);
        return new_leaf_idx;
}

void ui_dock_remove_tab(UIDockState *dock, int tab_id)
{
        if (!dock)
                return;

        int leaves[UI_DOCK_MAX_NODES];
        int count = 0;
        ui_dock_collect_leaves(dock, dock->root, leaves, &count, UI_DOCK_MAX_NODES);
        for (int i = 0; i < count; i++)
                if (ui_dock_remove_tab_from_leaf(dock, leaves[i], tab_id))
                        return;
}

bool ui_dock_is_animating(const UIDockState *dock)
{
        if (!dock)
                return false;

        if (dock->dragging_tab >= 0 || dock->pending_tab >= 0 || dock->switch_flash > 0.0f)
                return true;

        for (int i = 0; i < UI_DOCK_MAX_NODES; i++)
                if (dock->nodes[i].in_use && dock->nodes[i].kind == UI_DOCK_NODE_SPLIT_H && dock->nodes[i].dragging_splitter)
                        return true;
        return false;
}

void ui_dock_begin_frame(UIDockState *dock, int x, int y, int w, int h)
{
        if (!dock)
                return;

        int ox = active_view ? active_view->x : 0;
        int oy = active_view ? active_view->y : 0;
        dock->bounds = (View){x + ox, y + oy, w, h};
        ui_dock_layout_node(dock, dock->root, dock->bounds);

        if (!ui_dock_is_leaf_node(dock, dock->active_leaf))
                dock->active_leaf = ui_dock_first_leaf_from(dock, dock->root);

        static int last_mx = -1, last_my = -1;
        bool mouse_moved = (term_mouse.x != last_mx || term_mouse.y != last_my);
        last_mx = term_mouse.x;
        last_my = term_mouse.y;

        if (term_mouse.clicked || term_mouse.right_clicked || term_mouse.wheel != 0)
        {
                int leaf = ui_dock_leaf_at_point(dock, dock->root, term_mouse.x, term_mouse.y);
                if (leaf >= 0 && leaf != dock->active_leaf)
                {
                        dock->active_leaf = leaf;
                        global_ctx.active = false;
                        global_ctx.w = 0;
                }
        }
}

bool ui_dock_leaf_get(const UIDockState *dock, int leaf_idx, View *out_view, int *out_active_tab, bool *out_active)
{
        if (!ui_dock_is_leaf_node(dock, leaf_idx))
                return false;
        const UIDockNode *leaf = &dock->nodes[leaf_idx];
        if (out_view)
                *out_view = leaf->view;
        if (out_active_tab)
                *out_active_tab = leaf->active_tab;
        if (out_active)
                *out_active = (leaf_idx == dock->active_leaf);
        return true;
}

int ui_dock_take_close_request(UIDockState *dock)
{
        int result = dock ? dock->close_request_tab : -1;
        if (dock)
                dock->close_request_tab = -1;
        return result;
}

int ui_dock_take_add_request_leaf(UIDockState *dock)
{
        int result = dock ? dock->add_request_leaf : -1;
        if (dock)
                dock->add_request_leaf = -1;
        return result;
}

void ui_dock_draw(UIDockState *dock, const UITab *tabs, int tab_count, Color bar_bg, Color active_bg, Color divider_fg, Color bg)
{
        if (!dock)
                return;
        (void)tab_count;

        if (dock->last_active_leaf != dock->active_leaf)
        {
                dock->switch_flash = 1.0f;
                dock->last_active_leaf = dock->active_leaf;
        }
        else if (dock->switch_flash > 0.0f)
        {
                extern float term_dt_scale;
                dock->switch_flash -= 0.1f * term_dt_scale;
                if (dock->switch_flash < 0.0f)
                        dock->switch_flash = 0.0f;
        }

        int leaves[UI_DOCK_MAX_NODES];
        int leaf_count = 0;
        ui_dock_collect_leaves(dock, dock->root, leaves, &leaf_count, UI_DOCK_MAX_NODES);

        for (int leaf_ord = 0; leaf_ord < leaf_count; leaf_ord++)
        {
                int leaf_idx = leaves[leaf_ord];
                UIDockNode *leaf = &dock->nodes[leaf_idx];

                ui_set_view(&leaf->view);
                ui_suppress_mouse(leaf_idx != dock->active_leaf);

                UITab leaf_tabs[UI_DOCK_MAX_TABS];
                for (int i = 0; i < leaf->tab_count; i++)
                {
                        int tab_id = leaf->tabs[i];
                        leaf_tabs[i] = tabs[tab_id];
                        leaf_tabs[i].active = (tab_id == leaf->active_tab);
                        leaf_tabs[i].closable = (leaf->tab_count > 1 || leaf_count > 1);
                }

                UITabBarResult bar = ui_tab_bar(leaf->view.w, leaf_tabs, leaf->tab_count, true, leaf_idx == dock->active_leaf, leaf_idx == dock->active_leaf ? dock->switch_flash : 0.0f);
                if (leaf_idx == dock->active_leaf)
                {
                        if (bar.close_tab >= 0)
                                dock->close_request_tab = leaf->tabs[bar.close_tab];
                        if (bar.clicked_tab >= 0)
                        {
                                leaf->active_tab = leaf->tabs[bar.clicked_tab];
                                dock->pending_tab = leaf->active_tab;
                                dock->pending_leaf = leaf_idx;
                                dock->pending_start_x = term_mouse.x;
                                dock->pending_frames = 0;
                        }
                        if (bar.add_clicked)
                                dock->add_request_leaf = leaf_idx;

                        Color corner_bg = (Color){255, 255, 255};

                        ui_text(leaf->view.w - 1, 0, " ", corner_bg, corner_bg, false, false);
                        ui_text(0, leaf->view.h - 1, " ", corner_bg, corner_bg, false, false);
                        ui_text(leaf->view.w - 1, leaf->view.h - 1, " ", corner_bg, corner_bg, false, false);
                }
        }

        ui_set_view(NULL);
        ui_suppress_mouse(false);

        if (dock->pending_tab >= 0)
        {
                if (!term_mouse.left)
                        dock->pending_tab = -1;
                else
                {
                        dock->pending_frames++;
                        if (dock->pending_frames >= 2 && abs(term_mouse.x - dock->pending_start_x) > 2)
                        {
                                dock->dragging_tab = dock->pending_tab;
                                dock->drag_src_leaf = dock->pending_leaf;
                                dock->drag_start_x = dock->pending_start_x;
                                dock->pending_tab = -1;
                        }
                }
        }

        if (dock->dragging_tab >= 0)
        {
                if (!term_mouse.left)
                {
                        int src = dock->drag_src_leaf;
                        int dst = ui_dock_leaf_at_point(dock, dock->root, term_mouse.x, term_mouse.y);
                        UIDockDropKind drop_kind = ui_dock_drop_kind_for_leaf(dock, dst, src, term_mouse.x);

                        if (drop_kind == UI_DOCK_DROP_MERGE)
                        {
                                ui_dock_remove_tab(dock, dock->dragging_tab);
                                ui_dock_add_tab_to_leaf(dock, dst, dock->dragging_tab);
                                dock->active_leaf = dst;
                        }
                        else if ((drop_kind == UI_DOCK_DROP_SPLIT_LEFT || drop_kind == UI_DOCK_DROP_SPLIT_RIGHT) && abs(term_mouse.x - dock->drag_start_x) > 5)
                        {
                                int new_leaf = ui_dock_split_leaf(dock, dst, drop_kind == UI_DOCK_DROP_SPLIT_LEFT);
                                if (new_leaf >= 0)
                                {
                                        ui_dock_remove_tab(dock, dock->dragging_tab);
                                        ui_dock_add_tab_to_leaf(dock, new_leaf, dock->dragging_tab);
                                        dock->active_leaf = new_leaf;
                                }
                        }
                        dock->dragging_tab = -1;
                }
                else
                {
                        int src = dock->drag_src_leaf;
                        int dst = ui_dock_leaf_at_point(dock, dock->root, term_mouse.x, term_mouse.y);
                        UIDockDropKind drop_kind = ui_dock_drop_kind_for_leaf(dock, dst, src, term_mouse.x);
                        View preview = ui_dock_preview_rect_for_drop(dock, dst, drop_kind);

                        if (preview.w > 0 && preview.h > 0)
                        {
                                Color overlay = (Color){40, 80, 160};
                                for (int row = preview.y; row < preview.y + preview.h; row++)
                                {
                                        if (preview.x >= 0 && preview.x < term_width && row >= 0 && row < term_height)
                                                canvas[row * term_width + preview.x].bg = overlay;
                                        int rx = preview.x + preview.w - 1;
                                        if (rx >= 0 && rx < term_width && row >= 0 && row < term_height)
                                                canvas[row * term_width + rx].bg = overlay;
                                }
                                for (int col = preview.x; col < preview.x + preview.w && col < term_width; col++)
                                        if (col >= 0)
                                        {
                                                if (preview.y >= 0 && preview.y < term_height)
                                                        canvas[preview.y * term_width + col].bg = overlay;
                                                int by = preview.y + preview.h - 1;
                                                if (by >= 0 && by < term_height)
                                                        canvas[by * term_width + col].bg = overlay;
                                        }
                                const char *label = tabs[dock->dragging_tab].label ? tabs[dock->dragging_tab].label : "";
                                int len = 0;
                                for (const char *c = label; *c; c++)
                                        len++;
                                ui_text(preview.x + (preview.w - len) / 2, preview.y + preview.h / 2, label, (Color){255, 255, 255}, overlay, true, false);
                        }

                        const char *label = tabs[dock->dragging_tab].label ? tabs[dock->dragging_tab].label : "";
                        int len = 0;
                        for (const char *c = label; *c; c++)
                                len++;
                        int tw = len + 2;
                        int tx = term_mouse.x - tw / 2;
                        int ty = term_mouse.y;
                        if (ty >= dock->bounds.y && ty < dock->bounds.y + dock->bounds.h)
                        {
                                for (int col = tx; col < tx + tw && col < term_width; col++)
                                        if (col >= 0)
                                                canvas[ty * term_width + col] = (Cell){" ", (Color){255, 255, 255}, active_bg, true, false};
                                ui_text(tx + 1, ty, label, (Color){255, 255, 255}, active_bg, true, false);
                        }
                }
        }

        if (dock->dragging_tab < 0)
                ui_dock_draw_splitters(dock, dock->root, divider_fg, bg);
}

void ui_end(void)
{

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

        if (strcmp(current_cursor, next_cursor) != 0)
        {
                current_cursor = next_cursor;
                printf("\033]22;%s\007", current_cursor);
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

        int last_mouse_x = s->last_mouse_x;
        int last_mouse_y = s->last_mouse_y;
        bool ignore_mouse = s->ignore_mouse;

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

        s->last_mouse_x = last_mouse_x;
        s->last_mouse_y = last_mouse_y;
        s->ignore_mouse = ignore_mouse;

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

        if (s->is_box_selecting && s->active_box_selections && s->selections_cap > 0)
                memset(s->active_box_selections, 0, s->selections_cap * sizeof(bool));

        if (ui_get_mouse().x != s->last_mouse_x || ui_get_mouse().y != s->last_mouse_y)
        {
                s->ignore_mouse = false;
                s->last_mouse_x = ui_get_mouse().x;
                s->last_mouse_y = ui_get_mouse().y;
        }

        if (ui_get_mouse().left || ui_get_mouse().right || ui_get_mouse().wheel != 0)
                s->ignore_mouse = false;

        if ((key >= KEY_UP && key <= KEY_SHIFT_PAGE_DOWN) || key == '\t' || key == ' ' || key == KEY_ENTER || key == KEY_BACKSPACE)
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
                        UIRect kb_rect = ui_list_item_rect(s, s->selected_idx);
                        s->kb_drag_x = kb_rect.x;
                        s->kb_drag_y = kb_rect.y;
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

        if (ui_get_mouse().clicked && ui_get_mouse().x == s->p.x + s->p.w - 1 && ui_get_mouse().y >= s->p.y && ui_get_mouse().y < s->p.y + s->p.h)
        {
                s->dragging_scroll = true;
                int thumb_top = s->p.y + (max_scroll > 0 ? (int)((s->current_scroll / max_scroll) * (s->p.h - thumb_h)) : 0);
                s->drag_offset = (ui_get_mouse().y >= thumb_top && ui_get_mouse().y < thumb_top + thumb_h) ? (float)(ui_get_mouse().y - thumb_top) : (float)thumb_h / 2.0f;
        }

        if (!ui_get_mouse().left)
                s->dragging_scroll = false;

        if (s->dragging_scroll && max_scroll > 0)
        {
                float cr = (float)(ui_get_mouse().y - s->p.y - s->drag_offset) / (s->p.h - thumb_h > 0 ? s->p.h - thumb_h : 1);
                s->target_scroll = s->current_scroll = (cr < 0 ? 0 : cr > 1 ? 1
                                                                            : cr) *
                                                       max_scroll;
                s->scroll_velocity = 0.0f;
        }

        if (ui_get_mouse().wheel != 0)
        {
                float base_power = (s->mode == UI_MODE_LIST) ? 0.18f : ((float)c_h * 0.1f);
                float wheel_dir = ui_get_mouse().wheel > 0 ? 1.0f : -1.0f;
                float vel_dir = s->scroll_velocity > 0 ? 1.0f : (s->scroll_velocity < 0 ? -1.0f : 0.0f);

                if (vel_dir != 0.0f && vel_dir != wheel_dir)
                        s->scroll_velocity = 0.0f;
                if (vel_dir == wheel_dir && ui_fabsf(s->scroll_velocity) > 0.05f)
                        base_power *= (2.5f + ui_fabsf(s->scroll_velocity) * 1.2f);
                s->scroll_velocity += ui_get_mouse().wheel * base_power;
        }

        s->target_scroll += s->scroll_velocity * term_dt_scale;
        s->scroll_velocity *= ui_powf(0.82f, term_dt_scale);

        if (s != &global_ctx.list && (s->dragging_scroll || ui_get_mouse().wheel != 0))
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
                if (max_scroll - s->target_scroll < c_h / 2.0f)
                        snap = max_scroll;
                else if (snap > max_scroll)
                        snap = max_scroll;
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
                int bx = s->box_start_x < ui_get_mouse().x ? s->box_start_x : ui_get_mouse().x;
                int bw = abs(ui_get_mouse().x - s->box_start_x) + 1;
                float world_by = s->box_start_y_world < (ui_get_mouse().y + s->current_scroll) ? s->box_start_y_world : (ui_get_mouse().y + s->current_scroll);
                float world_bh = ui_fabsf((ui_get_mouse().y + s->current_scroll) - s->box_start_y_world) + 1;

                if (r.x < bx + bw && r.x + r.w > bx && item_world_y < world_by + world_bh && item_world_y + r.h > world_by)
                        s->active_box_selections[index] = true;
        }

        if (r.y + r.h <= s->p.y || r.y >= s->p.y + s->p.h)
                return false;
        *res = (UIItemResult){.x = r.x, .y = r.y, .w = r.w, .h = r.h};

        bool over_ctx_menu = global_ctx.active && (s != &global_ctx.list) && ui_get_mouse().x >= global_ctx.x && ui_get_mouse().x < global_ctx.x + global_ctx.w && ui_get_mouse().y >= global_ctx.y && ui_get_mouse().y < global_ctx.y + global_ctx.h;
        res->hovered = (!s->ignore_mouse && !over_ctx_menu && !s->dragging_scroll && !s->is_box_selecting && ui_get_mouse().x >= r.x && ui_get_mouse().x < r.x + r.w && ui_get_mouse().y >= r.y && ui_get_mouse().y < r.y + r.h && ui_get_mouse().y >= s->p.y && ui_get_mouse().y < s->p.y + s->p.h && ui_get_mouse().x < s->p.x + s->p.w - 1);

        bool is_hit = (s->mode == UI_MODE_GRID) ? !(ui_get_mouse().x == r.x || ui_get_mouse().x == r.x + r.w - 1 || ui_get_mouse().y == r.y || ui_get_mouse().y == r.y + r.h - 1) : (ui_get_mouse().x <= r.x + 35);

        if (res->hovered && is_hit)
                ui_set_cursor("pointer");

        res->pressed = (res->hovered && ui_get_mouse().left);
        res->clicked = (res->hovered && ui_get_mouse().clicked);
        res->right_clicked = (res->hovered && ui_get_mouse().right_clicked);

        if (res->clicked || res->right_clicked)
        {
                s->selected_idx = index;
                if (is_hit)
                {
                        s->clicked_on_item = true;
                        if (res->clicked)
                        {
                                if (ui_get_mouse().ctrl)
                                {
                                        s->selections[index] = !s->selections[index];
                                }
                                else if (!s->selections[index])
                                {
                                        ui_list_clear_selections(s);
                                        s->selections[index] = true;
                                }
                        }
                        else if (res->right_clicked && !s->selections[index])
                        {
                                ui_list_clear_selections(s);
                                s->selections[index] = true;
                        }
                }
        }

        if (res->hovered && ui_get_mouse().clicked && is_hit && s->drag_idx == -1 && !s->dragging_scroll && !global_ctx.active)
        {
                s->drag_idx = index;
                s->drag_start_x = ui_get_mouse().x;
                s->drag_start_y = ui_get_mouse().y;
                s->drag_off_x = ui_get_mouse().x - r.x;
                s->drag_off_y = ui_get_mouse().y - r.y;
        }

        res->is_selected = s->selections[index] || s->active_box_selections[index];
        res->is_ghost = (s->is_dragging && s->drag_idx == index) || (s->is_kb_dragging && s->kb_drag_idx == index);
        res->is_drop_target = ((s->is_dragging || s->external_drag) && res->hovered && index != s->drag_idx) || (s->is_kb_dragging && s->selected_idx == index && index != s->kb_drag_idx);

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

        if (ui_get_mouse().clicked && !s->clicked_on_item && !s->dragging_scroll && !global_ctx.active)
        {
                if (ui_get_mouse().x >= s->p.x && ui_get_mouse().x < s->p.x + s->p.w - 1 && ui_get_mouse().y >= s->p.y && ui_get_mouse().y < s->p.y + s->p.h)
                {
                        s->selected_idx = -1;
                        if (!ui_get_mouse().ctrl)
                                ui_list_clear_selections(s);
                        s->is_box_selecting = true;
                        s->box_start_x = ui_get_mouse().x;
                        s->box_start_y_world = ui_get_mouse().y + s->current_scroll;
                }
        }

        if (s->is_box_selecting)
        {
                if (!ui_get_mouse().left)
                {
                        if (ui_mouse_in_active_view())
                        {
                                for (int i = 0; i < s->p.item_count; i++)
                                        if (s->active_box_selections[i])
                                                s->selections[i] = true;
                        }
                        else if (s->active_box_selections && s->selections_cap > 0)
                        {
                                memset(s->active_box_selections, 0, s->selections_cap * sizeof(bool));
                        }
                        s->is_box_selecting = false;
                }
                else if (ui_mouse_in_active_view())
                {
                        int start_screen_y = (int)(s->box_start_y_world - s->current_scroll);
                        int curr_screen_y = ui_get_mouse().y;
                        int bx = s->box_start_x < ui_get_mouse().x ? s->box_start_x : ui_get_mouse().x;
                        int by = start_screen_y < curr_screen_y ? start_screen_y : curr_screen_y;
                        int bw = abs(ui_get_mouse().x - s->box_start_x) + 1;
                        int bh = abs(curr_screen_y - start_screen_y) + 1;
                        Color box_clr = {60, 100, 180};
                        int vox = active_view ? active_view->x : 0;
                        int voy = active_view ? active_view->y : 0;

                        for (int x = bx; x < bx + bw; x++)
                        {
                                int ax = x + vox;
                                if (ax >= 0 && ax < term_width && by + voy >= s->p.y && by + voy < s->p.y + s->p.h)
                                        canvas[(by + voy) * term_width + ax].bg = box_clr;
                                if (ax >= 0 && ax < term_width && by + bh - 1 + voy >= s->p.y && by + bh - 1 + voy < s->p.y + s->p.h)
                                        canvas[(by + bh - 1 + voy) * term_width + ax].bg = box_clr;
                        }
                        for (int y = by; y < by + bh; y++)
                        {
                                int ay = y + voy;
                                if (bx + vox >= 0 && bx + vox < term_width && ay >= s->p.y && ay < s->p.y + s->p.h)
                                        canvas[ay * term_width + bx + vox].bg = box_clr;
                                if (bx + bw - 1 + vox >= 0 && bx + bw - 1 + vox < term_width && ay >= s->p.y && ay < s->p.y + s->p.h)
                                        canvas[ay * term_width + bx + bw - 1 + vox].bg = box_clr;
                        }
                }
                else if (s->active_box_selections && s->selections_cap > 0)
                {
                        memset(s->active_box_selections, 0, s->selections_cap * sizeof(bool));
                }
        }

        if (max_scroll > 0)
        {
                bool hover = (!s->dragging_scroll && ui_get_mouse().x == s->p.x + s->p.w - 1 && ui_get_mouse().y >= s->p.y && ui_get_mouse().y < s->p.y + s->p.h);

                int thumb_h_half = (s->p.h * 2) * s->p.h / (rows * c_h);
                if (thumb_h_half < 2)
                        thumb_h_half = 2;
                int thumb_top_half = (int)((s->current_scroll / max_scroll) * (s->p.h * 2 - thumb_h_half));
                int thumb_bot_half = thumb_top_half + thumb_h_half;

                int my = ui_get_mouse().y - s->p.y;
                int my_top = my * 2, my_bot = my * 2 + 1;
                bool thumb_hover = hover && ((my_top >= thumb_top_half && my_top < thumb_bot_half) || (my_bot >= thumb_top_half && my_bot < thumb_bot_half));

                Color thumb_col = (s->dragging_scroll || thumb_hover) ? (Color){255, 255, 255} : s->p.scrollbar_fg;

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
                if (ui_get_mouse().left)
                {
                        if (!s->is_dragging && (ui_fabsf(ui_get_mouse().x - s->drag_start_x) > 1 || ui_fabsf(ui_get_mouse().y - s->drag_start_y) > 1))
                        {
                                s->is_dragging = true;
                                s->pickup_anim = 1.0f;
                                s->carry_x = ui_get_mouse().x - (s->mode == UI_MODE_LIST ? 2 : s->drag_off_x);
                                s->carry_y = ui_get_mouse().y - (s->mode == UI_MODE_LIST ? 1 : s->drag_off_y);
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
        if (s->is_dragging)
        {
                s->carry_x = ui_get_mouse().x - (s->mode == UI_MODE_LIST ? 2 : s->drag_off_x);
                s->carry_y = ui_get_mouse().y - (s->mode == UI_MODE_LIST ? 1 : s->drag_off_y);
        }
        else if (s->carrying)
        {
                if (!s->ignore_mouse)
                {
                        s->carry_x += (ui_get_mouse().x - s->carry_x) * ANIM_SPEED_CARRY;
                        s->carry_y += (ui_get_mouse().y - s->carry_y) * ANIM_SPEED_CARRY;
                }
                else
                {
                        float tx = sel_x + (s->mode == UI_MODE_LIST ? 45 : s->p.cell_w / 2);
                        float ty = sel_y + (s->mode == UI_MODE_LIST ? -1 : s->p.cell_h / 2);
                        s->carry_x += (tx - s->carry_x) * ANIM_SPEED_CARRY;
                        s->carry_y += (ty - s->carry_y) * ANIM_SPEED_CARRY;
                }
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
                float fx = is_dropped ? (s->drop_to_target ? s->drop_dst_x + (s->carry_x - s->drop_dst_x) * e : base_x + (s->carry_x - base_x) * e) : s->carry_x + (base_x - s->carry_x) * e;
                float fy = is_dropped ? (s->drop_to_target ? s->drop_dst_y + (s->carry_y - s->drop_dst_y) * e : base_y + (s->carry_y - base_y) * e) : s->carry_y + (base_y - s->carry_y) * e;
                *out_x = (int)(fx + (fx >= 0 ? 0.5f : -0.5f));
                *out_y = (int)(fy + (fy >= 0 ? 0.5f : -0.5f));
                return true;
        }
        return false;
}

bool ui_list_get_fly_coords(UIListState *s, int *out_x, int *out_y)
{
        if (s->fly_anim > 0.0f)
        {
                float ease = s->fly_anim * s->fly_anim;
                float fx = s->fly_is_pickup ? s->carry_x + (s->fly_origin_x - s->carry_x) * ease : s->fly_origin_x + (s->carry_x - s->fly_origin_x) * ease;
                float fy = s->fly_is_pickup ? s->carry_y + (s->fly_origin_y - s->carry_y) * ease : s->fly_origin_y + (s->carry_y - s->fly_origin_y) * ease;
                *out_x = (int)(fx + (fx >= 0 ? 0.5f : -0.5f));
                *out_y = (int)(fy + (fy >= 0 ? 0.5f : -0.5f));
                return true;
        }
        return false;
}

void ui_context_open(void *id, int target_idx)
{
        global_ctx = (UIContextState){.active = true, .id = id, .x = ui_get_mouse().x, .y = ui_get_mouse().y, .w = 0};
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

bool ui_context_do(void *id, const char **items, int count, int *out_idx)
{
        if (!global_ctx.active || global_ctx.id != id)
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

                int vw = active_view ? active_view->w : term_width;
                int vh = active_view ? active_view->h : term_height;

                if (global_ctx.x + global_ctx.w > vw)
                        global_ctx.x = (vw - global_ctx.w < 0) ? 0 : vw - global_ctx.w;
                if (global_ctx.y + global_ctx.h > vh)
                {
                        global_ctx.h = vh - global_ctx.y;
                        if (global_ctx.h < 4 && count >= 4)
                        {
                                global_ctx.y = (vh - count < 0) ? 0 : vh - count;
                                global_ctx.h = (count > vh) ? vh : count;
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

        if ((ui_get_mouse().clicked || ui_get_mouse().right_clicked) && !action_taken && !global_ctx.list.dragging_scroll)
                if (!(ui_get_mouse().x >= global_ctx.x && ui_get_mouse().x < global_ctx.x + params.w && ui_get_mouse().y >= global_ctx.y && ui_get_mouse().y < global_ctx.y + params.h))
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
                else if (key >= 32 && key <= 126 && (size_t)len + 1 < buf_size)
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