#include "terminal.c"
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <ctype.h>

Color clr_bg = {-1, -1, -1}, clr_bar = {170, 170, 170}, clr_text = {255, 255, 255}, clr_folder = {255, 255, 85}, clr_hover = {170, 170, 170}, clr_sel_bg = {40, 70, 120};

typedef struct AppState AppState;

char (*global_clipboard)[PATH_MAX] = NULL;
int global_clipboard_count = 0;
int global_clipboard_cap = 0;

typedef struct
{
        char old_path[PATH_MAX];
        char new_path[PATH_MAX];
} FileMove;

typedef struct
{
        FileMove *moves;
        int count;
        AppState *app;
} MoveAction;

typedef struct
{
        char name[256];
        bool is_dir, is_exec;
        off_t size;
        char git_status[3];
} FileEntry;

typedef struct
{
        FileEntry entry;
        char path[PATH_MAX];
} CarriedFile;

typedef struct
{
        char src_path[PATH_MAX];
        char dst_path[PATH_MAX];
} CopyMove;

typedef struct
{
        CopyMove *moves;
        int count;
        AppState *app;
} CopyAction;

struct AppState
{
        FileEntry *entries;
        int capacity, count;
        char cwd[PATH_MAX], next_dir[PATH_MAX];
        UIListState list;
        bool quit;

        CarriedFile *carried;
        int carried_count, carried_cap;

        char (*drop_paths)[PATH_MAX];
        int drop_count, drop_cap;

        int last_hovered_idx;
        FileEntry fly_entry;

        char (*pop_paths)[PATH_MAX];
        int pop_count, pop_cap;
        float pop_anim;
        bool pop_is_out;

        char trash_dir[PATH_MAX];
        int trash_counter;

        long long last_mtime;
        long long last_mtime_ns;

        char git_branch[64];
};

#define MAX_TABS 8

typedef struct
{
        AppState app;
        bool in_use;
} AppTab;

extern AppTab tabs[MAX_TABS];
extern UIDockState dock;
int add_tab(const char *dir);

void get_dir_mtime(const char *path, long long *sec, long long *ns)
{
        struct stat st;
        if (stat(path, &st) == 0)
        {
                *sec = st.st_mtime;
#ifdef __APPLE__
                *ns = st.st_mtimespec.tv_nsec;
#else
                *ns = st.st_mtim.tv_nsec;
#endif
        }
        else
        {
                *sec = 0;
                *ns = 0;
        }
}

void rm_rf(const char *path)
{
        struct stat st;
        if (stat(path, &st) != 0)
                return;
        if (S_ISDIR(st.st_mode))
        {
                DIR *d = opendir(path);
                if (d)
                {
                        struct dirent *dir;
                        while ((dir = readdir(d)) != NULL)
                        {
                                if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, ".."))
                                        continue;
                                char sub[PATH_MAX];
                                snprintf(sub, PATH_MAX, "%s/%s", path, dir->d_name);
                                rm_rf(sub);
                        }
                        closedir(d);
                }
                rmdir(path);
        }
        else
        {
                unlink(path);
        }
}

bool copy_path(const char *src, const char *dst)
{
        struct stat st;
        if (stat(src, &st) != 0)
                return false;

        if (S_ISDIR(st.st_mode))
        {
                mkdir(dst, 0777);
                DIR *d = opendir(src);
                if (!d)
                        return false;
                struct dirent *dir;
                while ((dir = readdir(d)) != NULL)
                {
                        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
                                continue;
                        char sub_src[PATH_MAX], sub_dst[PATH_MAX];
                        snprintf(sub_src, PATH_MAX, "%s/%s", src, dir->d_name);
                        snprintf(sub_dst, PATH_MAX, "%s/%s", dst, dir->d_name);
                        copy_path(sub_src, sub_dst);
                }
                closedir(d);
                chmod(dst, st.st_mode);
                return true;
        }
        else
        {
                FILE *fs = fopen(src, "rb");
                if (!fs)
                        return false;
                FILE *fd = fopen(dst, "wb");
                if (!fd)
                {
                        fclose(fs);
                        return false;
                }
                char buf[8192];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), fs)) > 0)
                        fwrite(buf, 1, n, fd);
                fclose(fs);
                fclose(fd);
                chmod(dst, st.st_mode);
                return true;
        }
}

bool move_path(const char *src, const char *dst)
{
        if (rename(src, dst) == 0)
                return true;
        if (copy_path(src, dst))
        {
                rm_rf(src);
                return true;
        }
        return false;
}

void cb_undo_move(void *data)
{
        MoveAction *act = (MoveAction *)data;
        act->app->pop_count = 0;
        act->app->pop_anim = 1.0f;

        char dir_old[PATH_MAX], dir_new[PATH_MAX];
        strcpy(dir_old, act->moves[0].old_path);
        char *slash_old = strrchr(dir_old, '/');
        if (slash_old)
                *slash_old = '\0';
        strcpy(dir_new, act->moves[0].new_path);
        char *slash_new = strrchr(dir_new, '/');
        if (slash_new)
                *slash_new = '\0';

        act->app->pop_is_out = !strcmp(act->app->cwd, dir_new);

        for (int i = 0; i < act->count; i++)
        {
                move_path(act->moves[i].new_path, act->moves[i].old_path);

                if (act->app->pop_count + 2 > act->app->pop_cap)
                {
                        act->app->pop_cap = act->app->pop_cap ? act->app->pop_cap * 2 : 64;
                        act->app->pop_paths = realloc(act->app->pop_paths, act->app->pop_cap * PATH_MAX) orelse return;
                }
                strcpy(act->app->pop_paths[act->app->pop_count++], act->moves[i].old_path);
                strcpy(act->app->pop_paths[act->app->pop_count++], act->moves[i].new_path);
        }
}

void cb_redo_move(void *data)
{
        MoveAction *act = (MoveAction *)data;
        act->app->pop_count = 0;
        act->app->pop_anim = 1.0f;

        char dir_old[PATH_MAX], dir_new[PATH_MAX];
        strcpy(dir_old, act->moves[0].old_path);
        char *slash_old = strrchr(dir_old, '/');
        if (slash_old)
                *slash_old = '\0';
        strcpy(dir_new, act->moves[0].new_path);
        char *slash_new = strrchr(dir_new, '/');
        if (slash_new)
                *slash_new = '\0';

        act->app->pop_is_out = !strcmp(act->app->cwd, dir_old);

        for (int i = 0; i < act->count; i++)
        {
                move_path(act->moves[i].old_path, act->moves[i].new_path);

                if (act->app->pop_count + 2 > act->app->pop_cap)
                {
                        act->app->pop_cap = act->app->pop_cap ? act->app->pop_cap * 2 : 64;
                        act->app->pop_paths = realloc(act->app->pop_paths, act->app->pop_cap * PATH_MAX) orelse return;
                }
                strcpy(act->app->pop_paths[act->app->pop_count++], act->moves[i].new_path);
                strcpy(act->app->pop_paths[act->app->pop_count++], act->moves[i].old_path);
        }
}

void cb_free_move(void *data)
{
        MoveAction *act = (MoveAction *)data;
        free(act->moves);
        free(act);
}

void cb_undo_copy(void *data)
{
        CopyAction *act = (CopyAction *)data;
        act->app->pop_count = 0;
        act->app->pop_anim = 1.0f;

        char dir_new[PATH_MAX];
        strcpy(dir_new, act->moves[0].dst_path);
        char *slash_new = strrchr(dir_new, '/');
        if (slash_new)
                *slash_new = '\0';
        act->app->pop_is_out = !strcmp(act->app->cwd, dir_new);

        for (int i = 0; i < act->count; i++)
        {
                rm_rf(act->moves[i].dst_path);
                if (act->app->pop_count + 1 > act->app->pop_cap)
                {
                        act->app->pop_cap = act->app->pop_cap ? act->app->pop_cap * 2 : 64;
                        act->app->pop_paths = realloc(act->app->pop_paths, act->app->pop_cap * PATH_MAX) orelse return;
                }
                strcpy(act->app->pop_paths[act->app->pop_count++], act->moves[i].dst_path);
        }
}

void cb_redo_copy(void *data)
{
        CopyAction *act = (CopyAction *)data;
        act->app->pop_count = 0;
        act->app->pop_anim = 1.0f;

        char dir_new[PATH_MAX];
        strcpy(dir_new, act->moves[0].dst_path);
        char *slash_new = strrchr(dir_new, '/');
        if (slash_new)
                *slash_new = '\0';
        act->app->pop_is_out = !strcmp(act->app->cwd, dir_new);

        for (int i = 0; i < act->count; i++)
        {
                copy_path(act->moves[i].src_path, act->moves[i].dst_path);
                if (act->app->pop_count + 1 > act->app->pop_cap)
                {
                        act->app->pop_cap = act->app->pop_cap ? act->app->pop_cap * 2 : 64;
                        act->app->pop_paths = realloc(act->app->pop_paths, act->app->pop_cap * PATH_MAX) orelse return;
                }
                strcpy(act->app->pop_paths[act->app->pop_count++], act->moves[i].dst_path);
        }
}

void cb_free_copy(void *data)
{
        CopyAction *act = (CopyAction *)data;
        free(act->moves);
        free(act);
}

void app_do_delete(AppState *app, int target_idx)
{
        UIListState *s = &app->list;
        bool drag_multi = false;

        if (target_idx != -1 && s->selections[target_idx])
        {
                for (int i = 0; i < app->count; i++)
                        if (s->selections[i])
                                drag_multi = true;
        }
        else if (target_idx == -1)
        {
                for (int i = 0; i < app->count; i++)
                        if (s->selections[i])
                                drag_multi = true;
                if (!drag_multi && s->selected_idx != -1)
                {
                        target_idx = s->selected_idx;
                }
        }

        int count = 0;
        int last_deleted = -1;
        for (int i = 0; i < app->count; i++)
        {
                if ((drag_multi && s->selections[i]) || (!drag_multi && i == target_idx))
                {
                        if (strcmp(app->entries[i].name, "..") != 0)
                        {
                                count++;
                                last_deleted = i;
                        }
                }
        }

        if (count == 0)
                return;

        MoveAction *act = malloc(sizeof(MoveAction)) orelse return;
        act->moves = malloc(sizeof(FileMove) * count) orelse
        {
                free(act);
                return;
        };
        act->count = 0;
        act->app = app;

        app->pop_count = 0;
        app->pop_anim = 1.0f;
        app->pop_is_out = true;

        for (int i = 0; i < app->count; i++)
        {
                if ((drag_multi && s->selections[i]) || (!drag_multi && i == target_idx))
                {
                        if (strcmp(app->entries[i].name, "..") != 0)
                        {
                                char src_path[PATH_MAX];
                                snprintf(src_path, PATH_MAX, "%s/%s", app->cwd, app->entries[i].name);

                                char dst_path[PATH_MAX];
                                snprintf(dst_path, PATH_MAX, "%s/trash_%d_%s", app->trash_dir, app->trash_counter++, app->entries[i].name);

                                if (move_path(src_path, dst_path))
                                {
                                        strcpy(act->moves[act->count].old_path, src_path);
                                        strcpy(act->moves[act->count].new_path, dst_path);
                                        act->count++;

                                        if (app->pop_count + 1 > app->pop_cap)
                                        {
                                                app->pop_cap = app->pop_cap ? app->pop_cap * 2 : 64;
                                                app->pop_paths = realloc(app->pop_paths, app->pop_cap * PATH_MAX) orelse continue;
                                        }
                                        strcpy(app->pop_paths[app->pop_count++], src_path);

                                        UIRect r = ui_list_item_rect(s, i);
                                        ui_burst_particles(r.x + r.w / 2, r.y + r.h / 2, 15, clr_text);
                                }
                        }
                }
        }

        int next_sel = -1;
        for (int i = last_deleted + 1; i < app->count; i++)
        {
                if (!drag_multi && i != target_idx)
                {
                        next_sel = i;
                        break;
                }
                if (drag_multi && !s->selections[i])
                {
                        next_sel = i;
                        break;
                }
        }
        if (next_sel == -1)
        {
                for (int i = last_deleted - 1; i >= 0; i--)
                {
                        if (!drag_multi && i != target_idx)
                        {
                                next_sel = i;
                                break;
                        }
                        if (drag_multi && !s->selections[i])
                        {
                                next_sel = i;
                                break;
                        }
                }
        }
        if (next_sel != -1)
                s->selected_idx = next_sel;

        if (act->count > 0)
        {
                ui_action_push(cb_undo_move, cb_redo_move, cb_free_move, act);
                strcpy(app->next_dir, ".");
        }
        else
        {
                cb_free_move(act);
        }
}

int cmp_entries(const void *a, const void *b)
{
        FileEntry *ea = (FileEntry *)a, *eb = (FileEntry *)b;
        if (ea->is_dir != eb->is_dir)
                return eb->is_dir - ea->is_dir;
        if (!strcmp(ea->name, ".."))
                return -1;
        if (!strcmp(eb->name, ".."))
                return 1;
        return strcmp(ea->name, eb->name);
}

void draw_item_grid(AppState *app, FileEntry *e, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target, bool is_multi_sel, bool is_dropped, bool is_popping, bool is_pressed, bool is_ctx_target)
{
        int float_y = 0;
        if (is_popping)
                float_y = app->pop_is_out ? (int)((1.0f - app->pop_anim) * 2.0f) : -(int)(app->pop_anim * 2.0f);
        y += float_y;

        Color item_bg = (is_drop_target && e->is_dir) ? (Color){50, 150, 50} : (is_ghost ? (is_sel ? (Color){60, 60, 60} : (Color){30, 30, 30}) : (is_multi_sel ? ((is_hover || is_sel) ? (Color){65, 105, 165} : clr_sel_bg) : ((is_hover || is_sel) ? clr_hover : clr_bg)));

        if (is_popping)
        {
                int flash = (int)(app->pop_anim * 80.0f);
                item_bg.r = item_bg.r + flash > 255 ? 255 : item_bg.r + flash;
                item_bg.g = item_bg.g + flash > 255 ? 255 : item_bg.g + flash;
                item_bg.b = item_bg.b + flash > 255 ? 255 : item_bg.b + flash;
        }

        // Safety check: ensure string exists before parsing index 1
        bool has_git = (e->git_status[0] != '\0');
        bool is_ignored = has_git && (e->git_status[0] == '!' && e->git_status[1] == '!');
        bool is_untracked = has_git && (e->git_status[0] == '?' && e->git_status[1] == '?');
        bool is_modified = has_git && (e->git_status[0] == 'M' || e->git_status[1] == 'M');
        bool is_added = has_git && (e->git_status[0] == 'A' || e->git_status[1] == 'A');

        Color base_text_clr = is_ignored ? (Color){100, 100, 100} : is_untracked ? (Color){85, 255, 255}
                                                                : is_modified    ? (Color){255, 170, 85}
                                                                : is_added       ? (Color){85, 255, 85}
                                                                                 : clr_text;

        Color icon_fg = is_ghost ? (is_sel ? (Color){200, 200, 200} : (Color){100, 100, 100}) : (is_ignored ? (Color){100, 100, 100} : (e->is_dir ? clr_folder : (e->is_exec ? (Color){85, 255, 85} : base_text_clr)));

        if (is_hover || is_sel || is_ghost || is_drop_target || is_multi_sel || is_popping)
                ui_rect(x + 1, y, w - 2, h, item_bg);

        char ext[5] = ".   ";
        if (!e->is_dir)
        {
                char *dot = strrchr(e->name, '.');
                if (dot && dot > e->name && strlen(dot + 1) < 4)
                        for (int j = 0; dot[1 + j]; j++)
                                ext[1 + j] = toupper(dot[1 + j]);
        }

        const char *icon_dir[] = {" ┌─┐____ ", " │ └────│ ", " │      │ ", " └──────┘ "};
        const char *icon_file[] = {"  ┌──┐_ ", "  │  └─│", "  │    │", "  └────┘"};
        const char *icon_exec[] = {"  ┌──┐_ ", "  │░░└─│", "  │░░░░│", "  └────┘"};
        const char **icon = e->is_dir ? icon_dir : (e->is_exec ? icon_exec : icon_file);

        int y_off = (is_hover && ui_get_mouse().left && !app->list.is_dragging) ? 1 : 0;
        for (int j = 0; j < 4; j++)
                ui_text(x + 2, y + j + y_off, icon[j], icon_fg, item_bg, false, false);
        if (ext[1] != ' ' && !e->is_dir)
                ui_text(x + 6, y + 2 + y_off, ext, icon_fg, item_bg, false, true);

        raw char l1[16];
        raw char l2[16];
        int mw = w - 2 > 15 ? 15 : (w - 2 < 0 ? 0 : w - 2);
        int len = strlen(e->name);

        strncpy(l1, e->name, mw);
        l1[mw] = '\0';
        l2[0] = '\0';

        if (len > mw)
        {
                strncpy(l2, e->name + mw, mw);
                l2[mw] = '\0';
                if (len > mw * 2)
                {
                        l2[mw - 2] = '.';
                        l2[mw - 1] = '.';
                }
        }
        ui_text_centered(x + 1, y + 4 + y_off, mw, l1, is_ghost ? icon_fg : base_text_clr, item_bg, is_dropped, false);
        if (l2[0])
                ui_text_centered(x + 1, y + 5 + y_off, mw, l2, is_ghost ? icon_fg : base_text_clr, item_bg, is_dropped, false);

        if (has_git && !is_ignored)
        {
                char badge[3] = {e->git_status[1] == ' ' ? e->git_status[0] : e->git_status[1], '\0'};
                ui_text(x + w - 3, y, badge, base_text_clr, item_bg, true, false);
        }

        float scale = 1.0f;
        if (is_popping)
                scale = app->pop_is_out ? app->pop_anim : (1.0f - app->pop_anim);
        else if (is_pressed)
                scale = 0.85f;
        else if (is_ctx_target)
                scale = 1.15f;

        if (scale < 0.99f || scale > 1.01f)
                ui_scale_region(x, y, w, h, scale, clr_bg);
}

void draw_item_list(AppState *app, FileEntry *e, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target, bool is_multi_sel, bool is_dropped, bool is_popping, bool is_pressed, bool is_ctx_target)
{
        int float_y = 0;
        if (is_popping)
                float_y = app->pop_is_out ? (int)((1.0f - app->pop_anim) * 1.0f) : -(int)(app->pop_anim * 1.0f);
        y += float_y;

        Color item_bg = (is_drop_target && e->is_dir) ? (Color){50, 150, 50} : (is_ghost ? (is_sel ? (Color){60, 60, 60} : (Color){30, 30, 30}) : (is_multi_sel ? ((is_hover || is_sel) ? (Color){65, 105, 165} : clr_sel_bg) : ((is_hover || is_sel) ? clr_hover : clr_bg)));

        if (is_popping)
        {
                int flash = (int)(app->pop_anim * 80.0f);
                item_bg.r = item_bg.r + flash > 255 ? 255 : item_bg.r + flash;
                item_bg.g = item_bg.g + flash > 255 ? 255 : item_bg.g + flash;
                item_bg.b = item_bg.b + flash > 255 ? 255 : item_bg.b + flash;
        }

        bool has_git = (e->git_status[0] != '\0');
        bool is_ignored = has_git && (e->git_status[0] == '!' && e->git_status[1] == '!');
        bool is_untracked = has_git && (e->git_status[0] == '?' && e->git_status[1] == '?');
        bool is_modified = has_git && (e->git_status[0] == 'M' || e->git_status[1] == 'M');
        bool is_added = has_git && (e->git_status[0] == 'A' || e->git_status[1] == 'A');

        Color base_text_clr = is_ignored ? (Color){100, 100, 100} : is_untracked ? (Color){85, 255, 255}
                                                                : is_modified    ? (Color){255, 170, 85}
                                                                : is_added       ? (Color){85, 255, 85}
                                                                                 : clr_text;

        Color icon_fg = is_ghost ? (is_sel ? (Color){200, 200, 200} : (Color){100, 100, 100}) : (is_ignored ? (Color){100, 100, 100} : (e->is_dir ? clr_folder : (e->is_exec ? (Color){85, 255, 85} : base_text_clr)));

        ui_rect(x, y, w, 1, item_bg);

        ui_text(x, y, e->is_dir ? "▓]" : "■ ", icon_fg, item_bg, false, false);

        int name_x = x + 3;

        if (has_git)
        {
                char badge[3] = {e->git_status[0] == ' ' ? '-' : e->git_status[0], e->git_status[1] == ' ' ? '-' : e->git_status[1], '\0'};
                ui_text(name_x, y, badge, base_text_clr, item_bg, false, false);
                name_x += 3;
        }

        int right_margin = (!e->is_dir) ? 8 : 1;
        int max_name_len = w - name_x - right_margin;
        if (max_name_len < 0) max_name_len = 0;
        int copy_len = max_name_len > 255 ? 255 : max_name_len;

        raw char l[256];
        strncpy(l, e->name, copy_len);
        l[copy_len] = '\0';

        ui_text(name_x, y, l, is_ghost ? icon_fg : base_text_clr, item_bg, is_dropped, false);

        raw char size_str[32];
        if (!e->is_dir)
        {
                off_t s = e->size;
                if (s < 1024)
                        snprintf(size_str, 32, "%5lld B", (long long)s);
                else if (s < 1024 * 1024)
                        snprintf(size_str, 32, "%4lld KB", (long long)(s >> 10));
                else if (s < 1024 * 1024 * 1024)
                        snprintf(size_str, 32, "%4lld MB", (long long)(s >> 20));
                else
                        snprintf(size_str, 32, "%4lld GB", (long long)(s >> 30));
                ui_text(x + w - 7, y, size_str, is_ghost ? icon_fg : clr_bar, item_bg, false, false);
        }

        float scale = 1.0f;
        if (is_popping)
                scale = app->pop_is_out ? app->pop_anim : (1.0f - app->pop_anim);
        else if (is_pressed)
                scale = 0.95f;
        else if (is_ctx_target)
                scale = 1.05f;

        if (scale < 0.99f || scale > 1.01f)
                ui_scale_region(x, y, w, h, scale, clr_bg);
}

void draw_item(AppState *app, FileEntry *e, UIItemResult *res, bool is_sel, bool is_ghost, bool is_dropped, bool is_popping, bool is_pressed, bool is_ctx_target)
{
        if (app->list.mode == UI_MODE_GRID)
                draw_item_grid(app, e, res->x, res->y, res->w, res->h, is_sel, res->hovered, is_ghost, res->is_drop_target, res->is_selected, is_dropped, is_popping, is_pressed, is_ctx_target);
        else
                draw_item_list(app, e, res->x, res->y, res->w, res->h, is_sel, res->hovered, is_ghost, res->is_drop_target, res->is_selected, is_dropped, is_popping, is_pressed, is_ctx_target);
}

bool is_item_dropped(AppState *app, const char *path)
{
        if (app->list.drop_anim <= 0.01f)
                return false;
        for (int d = 0; d < app->drop_count; d++)
                if (!strcmp(path, app->drop_paths[d]))
                        return true;
        return false;
}

bool is_item_carried(AppState *app, const char *path)
{
        if (!app->list.carrying)
                return false;
        for (int c = 0; c < app->carried_count; c++)
                if (!strcmp(path, app->carried[c].path))
                        return true;
        return false;
}

void app_load_dir(AppState *app, const char *path)
{
        bool dir_changed = (strcmp(path, ".") != 0);

        char prev_cwd[PATH_MAX];
        getcwd(prev_cwd, sizeof(prev_cwd)) orelse return;
        defer chdir(prev_cwd);

        if (app->cwd[0] && path[0] != '/')
                (chdir(app->cwd) == 0) orelse return;

        (chdir(path) == 0) orelse return;

        DIR *d = opendir(".");
        d orelse return;
        defer closedir(d);
        getcwd(app->cwd, sizeof(app->cwd)) orelse return;
        get_dir_mtime(".", &app->last_mtime, &app->last_mtime_ns);

        char sel[256] = "";
        if (app->count > 0 && app->list.selected_idx >= 0)
                strcpy(sel, app->entries[app->list.selected_idx].name);

        int saved_sel_count = 0;
        raw char (*saved_sels)[256] = NULL;
        if (app->count > 0 && app->list.selections)
        {
                saved_sels = malloc(app->count * 256);
                if (saved_sels)
                {
                        for (int i = 0; i < app->count; i++)
                        {
                                if (app->list.selections[i])
                                        strcpy(saved_sels[saved_sel_count++], app->entries[i].name);
                        }
                }
        }

        UIListMode m = app->list.mode;
        ui_list_reset(&app->list);
        app->list.mode = m;
        if (app->list.selections && app->list.selections_cap > 0)
                memset(app->list.selections, 0, app->list.selections_cap * sizeof(bool));
        if (app->list.active_box_selections && app->list.selections_cap > 0)
                memset(app->list.active_box_selections, 0, app->list.selections_cap * sizeof(bool));
        if (dir_changed)
        {
                app->list.drop_anim = 0.0f;
                app->list.fly_anim = 0.0f;
                app->list.pickup_anim = 0.0f;
                app->list.is_dragging = false;
                app->list.is_box_selecting = false;
                app->list.carrying = false;
                app->drop_count = 0;
        }
        app->last_hovered_idx = -1;
        app->count = 0;

        bool has_dot_dot = false;
        while (1)
        {
                struct dirent *dir = readdir(d);
                if (!dir) break;
                if (!strcmp(dir->d_name, "."))
                        continue;
                if (!strcmp(dir->d_name, ".."))
                        has_dot_dot = true;

                if (app->count >= app->capacity)
                {
                        app->capacity = app->capacity ? app->capacity * 2 : 256;
                        app->entries = realloc(app->entries, app->capacity * sizeof(FileEntry));
                        if (!app->entries) break;
                }

                raw struct stat st;
                memset(&st, 0, sizeof(st));
                if (stat(dir->d_name, &st))
                {
                        if (lstat(dir->d_name, &st))
                        {
                                if (strcmp(dir->d_name, "..") == 0)
                                        st.st_mode = S_IFDIR;
                                else
                                        continue;
                        }
                }

                FileEntry *e = &app->entries[app->count++];
                strcpy(e->name, dir->d_name);
                e->size = st.st_size;
                e->is_dir = S_ISDIR(st.st_mode);
                e->is_exec = (st.st_mode & S_IXUSR) && !e->is_dir;

                e->git_status[0] = '\0';
                e->git_status[1] = '\0';
                e->git_status[2] = '\0';
        }
        
        if (!has_dot_dot && strcmp(app->cwd, "/") != 0)
        {
                if (app->count >= app->capacity)
                {
                        app->capacity = app->capacity ? app->capacity * 2 : 256;
                        app->entries = realloc(app->entries, app->capacity * sizeof(FileEntry));
                }
                if (app->entries)
                {
                        FileEntry *e = &app->entries[app->count++];
                        strcpy(e->name, "..");
                        e->size = 0;
                        e->is_dir = true;
                        e->is_exec = false;
                        e->git_status[0] = '\0';
                        e->git_status[1] = '\0';
                        e->git_status[2] = '\0';
                }
        }
        
        if (app->entries)
                qsort(app->entries, app->count, sizeof(FileEntry), cmp_entries);

        if (!dir_changed && saved_sels)
        {
                for (int i = 0; i < app->count; i++)
                {
                        for (int j = 0; j < saved_sel_count; j++)
                        {
                                if (!strcmp(app->entries[i].name, saved_sels[j]))
                                {
                                        app->list.selections[i] = true;
                                        break;
                                }
                        }
                }
        }
        if (saved_sels)
                free(saved_sels);

        if (!dir_changed && app->list.drop_anim > 0.0f)
        {
                for (int i = 0; i < app->count; i++)
                {
                        raw char p[PATH_MAX];
                        snprintf(p, PATH_MAX, "%s/%s", app->cwd, app->entries[i].name);
                        for (int di = 0; di < app->drop_count; di++)
                        {
                                if (!strcmp(p, app->drop_paths[di]))
                                {
                                        app->list.selections[i] = true;
                                        app->list.selected_idx = i;
                                        break;
                                }
                        }
                }
        }

        if (dir_changed && app->count > 0)
                app->list.selected_idx = 0;
        else if (app->count > 0 && sel[0])
        {
                for (int i = 0; i < app->count; i++)
                {
                        if (!strcmp(app->entries[i].name, sel))
                        {
                                app->list.selected_idx = i;
                                break;
                        }
                }
        }

        app->git_branch[0] = '\0';
        FILE *f = popen("git branch --show-current 2>/dev/null", "r");
        if (f)
        {
                if (fgets(app->git_branch, sizeof(app->git_branch), f))
                {
                        char *nl = strchr(app->git_branch, '\n');
                        if (nl)
                                *nl = '\0';
                }
                pclose(f);
        }

        if (app->git_branch[0] != '\0')
        {
                f = popen("git --no-optional-locks status -s . 2>/dev/null", "r");
                if (f)
                {
                        char line[1024];
                        while (fgets(line, sizeof(line), f))
                        {
                                if (strlen(line) < 4)
                                        continue;
                                char status[3] = {line[0], line[1], '\0'};

                                char *p = line + 3;
                                if (p[0] == '"')
                                {
                                        p++;
                                        char *q = strrchr(p, '"');
                                        if (q)
                                                *q = '\0';
                                }
                                else
                                {
                                        char *nl = strchr(p, '\n');
                                        if (nl)
                                                *nl = '\0';
                                }

                                char *slash = strchr(p, '/');
                                if (slash)
                                        *slash = '\0';

                                for (int i = 0; i < app->count; i++)
                                {
                                        if (strcmp(app->entries[i].name, p) == 0)
                                        {
                                                if (app->entries[i].git_status[0] == '\0' || status[0] == 'M' || status[1] == 'M')
                                                {
                                                        strcpy(app->entries[i].git_status, status);
                                                }
                                                break;
                                        }
                                }
                        }
                        pclose(f);
                }
        }
}

void handle_input(AppState *app, int *key, const UIListParams *params)
{
        UIListState *s = &app->list;

        if (global_ctx.active && *key != KEY_ESC && *key != 0)
        {
                *key = 0;
                return;
        }

        if (*key == 1) // Ctrl+A
        {
                for (int i = 0; i < app->count; i++)
                {
                        if (strcmp(app->entries[i].name, "..") != 0)
                                s->selections[i] = true;
                }
                *key = 0;
        }

        if (*key == 'u' || *key == 26)
        {
                if (ui_action_undo())
                        strcpy(app->next_dir, ".");
                *key = 0;
        }
        if (*key == 'r' || *key == 'R' || *key == 25)
        {
                if (ui_action_redo())
                        strcpy(app->next_dir, ".");
                *key = 0;
        }

        if (*key == 'q' && !s->carrying)
                app->quit = true;

        // Toggle view with the '1' key
        if (*key == '1')
                ui_list_set_mode(s, params, !s->mode);

        if (*key == KEY_BACKSPACE)
                strcpy(app->next_dir, "..");

        if (*key == KEY_DELETE || *key == KEY_CTRL_BACKSPACE)
        {
                app_do_delete(app, -1);
                *key = 0;
        }

        if (*key == 4) // Ctrl+D -> Duplicate
        {
                bool drag_multi = false;
                for (int i = 0; i < app->count; i++)
                        if (s->selections[i])
                                drag_multi = true;

                int src = s->selected_idx != -1 ? s->selected_idx : (app->last_hovered_idx == -1 ? 0 : app->last_hovered_idx);

                int dup_count = 0;
                for (int i = 0; i < app->count; i++)
                        if ((drag_multi && s->selections[i]) || (!drag_multi && i == src))
                                if (strcmp(app->entries[i].name, "..") != 0)
                                        dup_count++;

                if (dup_count > 0)
                {
                        CopyAction *act = malloc(sizeof(CopyAction)) orelse return;
                        act->moves = malloc(sizeof(CopyMove) * dup_count) orelse
                        {
                                free(act);
                                return;
                        };
                        act->count = 0;
                        act->app = app;

                        app->pop_count = 0;
                        app->pop_anim = 1.0f;
                        app->pop_is_out = false;

                        for (int i = 0; i < app->count; i++)
                        {
                                if ((drag_multi && s->selections[i]) || (!drag_multi && i == src))
                                {
                                        if (strcmp(app->entries[i].name, "..") != 0)
                                        {
                                                char src_path[PATH_MAX];
                                                snprintf(src_path, PATH_MAX, "%s/%s", app->cwd, app->entries[i].name);

                                                char base_name[256];
                                                strncpy(base_name, app->entries[i].name, 255);
                                                base_name[255] = '\0';

                                                char *dot = strrchr(base_name, '.');
                                                char ext[256] = "";
                                                if (dot && dot != base_name)
                                                {
                                                        strcpy(ext, dot);
                                                        *dot = '\0';
                                                }

                                                char dst_path[PATH_MAX];
                                                char new_name[512];
                                                int copy_num = 0;
                                                struct stat st;
                                                do
                                                {
                                                        if (copy_num == 0)
                                                                snprintf(new_name, 512, "%s.copy%s", base_name, ext);
                                                        else
                                                                snprintf(new_name, 512, "%s.copy%d%s", base_name, copy_num, ext);
                                                        snprintf(dst_path, PATH_MAX, "%s/%s", app->cwd, new_name);
                                                        copy_num++;
                                                } while (stat(dst_path, &st) == 0);

                                                if (copy_path(src_path, dst_path))
                                                {
                                                        strcpy(act->moves[act->count].src_path, src_path);
                                                        strcpy(act->moves[act->count].dst_path, dst_path);
                                                        act->count++;

                                                        if (app->pop_count + 1 > app->pop_cap)
                                                        {
                                                                app->pop_cap = app->pop_cap ? app->pop_cap * 2 : 64;
                                                                app->pop_paths = realloc(app->pop_paths, app->pop_cap * PATH_MAX) orelse continue;
                                                        }
                                                        strcpy(app->pop_paths[app->pop_count++], dst_path);
                                                }
                                        }
                                }
                        }
                        if (act->count > 0)
                        {
                                ui_action_push(cb_undo_copy, cb_redo_copy, cb_free_copy, act);
                        }
                        else
                        {
                                cb_free_copy(act);
                        }
                        strcpy(app->next_dir, ".");
                }
                *key = 0;
        }

        if (*key == 'c' || *key == 3) // Ctrl+C or c
        {
                global_clipboard_count = 0;
                bool drag_multi = false;
                for (int i = 0; i < app->count; i++)
                        if (s->selections[i])
                                drag_multi = true;

                int src = s->selected_idx != -1 ? s->selected_idx : (app->last_hovered_idx == -1 ? 0 : app->last_hovered_idx);
                for (int i = 0; i < app->count; i++)
                {
                        if ((drag_multi && s->selections[i]) || (!drag_multi && i == src))
                        {
                                if (strcmp(app->entries[i].name, "..") != 0)
                                {
                                        if (global_clipboard_count >= global_clipboard_cap)
                                        {
                                                global_clipboard_cap = global_clipboard_cap ? global_clipboard_cap * 2 : 256;
                                                global_clipboard = realloc(global_clipboard, global_clipboard_cap * PATH_MAX) orelse break;
                                        }
                                        snprintf(global_clipboard[global_clipboard_count++], PATH_MAX, "%s/%s", app->cwd, app->entries[i].name);
                                }
                        }
                }
                ui_list_clear_selections(s);
                *key = 0;
        }

        if (*key == 'p' || *key == 22) // Ctrl+V or p
        {
                if (global_clipboard_count > 0)
                {
                        CopyAction *act = malloc(sizeof(CopyAction)) orelse return;
                        act->moves = malloc(sizeof(CopyMove) * global_clipboard_count) orelse
                        {
                                free(act);
                                return;
                        };
                        act->count = 0;
                        act->app = app;

                        app->pop_count = 0;
                        app->pop_anim = 1.0f;
                        app->pop_is_out = false;

                        for (int i = 0; i < global_clipboard_count; i++)
                        {
                                char *src_path = global_clipboard[i];
                                char *base = strrchr(src_path, '/');
                                base = base ? base + 1 : src_path;

                                char base_name[256];
                                strncpy(base_name, base, 255);
                                base_name[255] = '\0';

                                char *dot = strrchr(base_name, '.');
                                char ext[256] = "";

                                if (dot && dot != base_name)
                                {
                                        strcpy(ext, dot);
                                        *dot = '\0';
                                }

                                char dst_path[PATH_MAX];
                                char new_name[512];
                                int copy_num = 0;
                                struct stat st;
                                do
                                {
                                        if (copy_num == 0)
                                                snprintf(new_name, 512, "%s.copy%s", base_name, ext);
                                        else
                                                snprintf(new_name, 512, "%s.copy%d%s", base_name, copy_num, ext);
                                        snprintf(dst_path, PATH_MAX, "%s/%s", app->cwd, new_name);
                                        copy_num++;
                                } while (stat(dst_path, &st) == 0);

                                if (copy_path(src_path, dst_path))
                                {
                                        strcpy(act->moves[act->count].src_path, src_path);
                                        strcpy(act->moves[act->count].dst_path, dst_path);
                                        act->count++;

                                        if (app->pop_count + 1 > app->pop_cap)
                                        {
                                                app->pop_cap = app->pop_cap ? app->pop_cap * 2 : 64;
                                                app->pop_paths = realloc(app->pop_paths, app->pop_cap * PATH_MAX) orelse continue;
                                        }
                                        strcpy(app->pop_paths[app->pop_count++], dst_path);
                                }
                        }
                        if (act->count > 0)
                        {
                                ui_action_push(cb_undo_copy, cb_redo_copy, cb_free_copy, act);
                        }
                        else
                        {
                                cb_free_copy(act);
                        }
                        strcpy(app->next_dir, ".");
                }
                *key = 0;
        }

        if (*key == KEY_ESC)
        {
                bool has_selection = false;
                for (int i = 0; i < app->count; i++)
                        if (s->selections[i])
                        {
                                has_selection = true;
                                break;
                        }

                if (s->carrying || has_selection || global_ctx.active)
                {
                        s->carrying = false;
                        ui_list_clear_selections(s);
                        global_ctx.active = false;
                        global_ctx.w = 0;
                }
                else
                {
                        app->quit = true;
                }
                *key = 0;
        }

        if (*key == KEY_ENTER && app->count > 0 && s->selected_idx >= 0 && app->entries[s->selected_idx].is_dir)
        {
                raw char sel_path[PATH_MAX];
                snprintf(sel_path, PATH_MAX, "%s/%s", app->cwd, app->entries[s->selected_idx].name);
                if (!is_item_carried(app, sel_path))
                {
                        strcpy(app->next_dir, app->entries[s->selected_idx].name);
                }
                *key = 0;
        }

        if (*key == ' ' && s->carrying)
        {
                int src = s->selected_idx != -1 ? s->selected_idx : app->last_hovered_idx;
                if (src >= 0 && src < app->count && strcmp(app->entries[src].name, "..") != 0)
                {
                        raw char src_path[PATH_MAX];
                        snprintf(src_path, PATH_MAX, "%s/%s", app->cwd, app->entries[src].name);

                        int found_idx = -1;
                        for (int i = 0; i < app->carried_count; i++)
                                if (!strcmp(app->carried[i].path, src_path))
                                {
                                        found_idx = i;
                                        break;
                                }

                        UIRect r = ui_list_item_rect(s, src);
                        s->fly_origin_x = r.x;
                        s->fly_origin_y = r.y;
                        app->fly_entry = app->entries[src];
                        s->fly_anim = 1.0f;

                        if (found_idx != -1)
                        {
                                s->fly_is_pickup = false;
                                memmove(&app->carried[found_idx], &app->carried[found_idx + 1], (app->carried_count - found_idx - 1) * sizeof(CarriedFile));
                                if (--app->carried_count == 0)
                                        s->carrying = false;
                        }
                        else
                        {
                                if (app->carried_count >= app->carried_cap)
                                {
                                        app->carried_cap = app->carried_cap ? app->carried_cap * 2 : 256;
                                        app->carried = realloc(app->carried, app->carried_cap * sizeof(CarriedFile)) orelse
                                        {
                                                *key = 0;
                                                return;
                                        };
                                }
                                s->fly_is_pickup = true;
                                app->carried[app->carried_count].entry = app->entries[src];
                                strcpy(app->carried[app->carried_count++].path, src_path);
                        }
                }
                *key = 0;
        }

        if (*key == '\t')
        {
                if (s->carrying)
                {
                        app->drop_count = 0;
                        s->drop_anim = 1.0f;
                        s->drop_to_target = false;

                        MoveAction *act = malloc(sizeof(MoveAction)) orelse
                        {
                                *key = 0;
                                return;
                        };
                        act->moves = malloc(sizeof(FileMove) * app->carried_count) orelse
                        {
                                free(act);
                                *key = 0;
                                return;
                        };
                        act->count = 0;
                        act->app = app;

                        for (int i = 0; i < app->carried_count; i++)
                        {
                                raw char new_path[PATH_MAX];
                                snprintf(new_path, PATH_MAX, "%s/%s", app->cwd, app->carried[i].entry.name);

                                (move_path(app->carried[i].path, new_path)) orelse continue;

                                strcpy(act->moves[act->count].old_path, app->carried[i].path);
                                strcpy(act->moves[act->count].new_path, new_path);
                                act->count++;

                                if (app->drop_count >= app->drop_cap)
                                {
                                        app->drop_cap = app->drop_cap ? app->drop_cap * 2 : 64;
                                        app->drop_paths = realloc(app->drop_paths, app->drop_cap * PATH_MAX) orelse continue;
                                }
                                strcpy(app->drop_paths[app->drop_count++], new_path);
                        }

                        if (act->count > 0)
                                ui_action_push(cb_undo_move, cb_redo_move, cb_free_move, act);
                        else
                                cb_free_move(act);

                        s->carrying = false;
                        strcpy(app->next_dir, ".");
                }
                else
                {
                        app->carried_count = 0;
                        bool drag_multi = false;
                        for (int i = 0; i < app->count; i++)
                                if (s->selections[i])
                                        drag_multi = true;

                        int src = s->selected_idx != -1 ? s->selected_idx : (app->last_hovered_idx == -1 ? 0 : app->last_hovered_idx);
                        for (int i = 0; i < app->count; i++)
                        {
                                if ((drag_multi && s->selections[i]) || (!drag_multi && i == src))
                                {
                                        if (strcmp(app->entries[i].name, "..") != 0)
                                        {
                                                if (app->carried_count >= app->carried_cap)
                                                {
                                                        app->carried_cap = app->carried_cap ? app->carried_cap * 2 : 256;
                                                        app->carried = realloc(app->carried, app->carried_cap * sizeof(CarriedFile)) orelse break;
                                                }
                                                app->carried[app->carried_count].entry = app->entries[i];
                                                snprintf(app->carried[app->carried_count++].path, PATH_MAX, "%s/%s", app->cwd, app->entries[i].name);
                                        }
                                }
                        }
                        if (app->carried_count > 0)
                        {
                                s->carrying = true;
                                s->pickup_anim = 1.0f;

                                for (int i = 0; i < app->count; i++)
                                {
                                        if ((drag_multi && s->selections[i]) || (!drag_multi && i == src))
                                        {
                                                if (strcmp(app->entries[i].name, "..") != 0)
                                                {
                                                        UIRect br = ui_list_item_rect(s, i);
                                                        ui_burst_particles(br.x + br.w / 2, br.y + br.h / 2, drag_multi ? 5 : 15, clr_text);
                                                }
                                        }
                                }
                        }
                }
                *key = 0;
        }
}

void app_process_drops(AppState *app)
{
        UIListState *s = &app->list;

        if (s->action_click_idx != -1 && app->entries[s->action_click_idx].is_dir)
                strcpy(app->next_dir, app->entries[s->action_click_idx].name);

        (s->action_drop_src != -1) orelse return;

        int src = s->action_drop_src, dst = s->action_drop_dst;
        bool drag_multi = false;

        if (src >= 0 && s->selections[src])
                for (int i = 0; i < app->count; i++)
                        if (s->selections[i])
                                drag_multi = true;

        CarriedFile *temp_carried = malloc(sizeof(CarriedFile) * app->count) orelse return;
        defer free(temp_carried);
        int temp_carried_count = 0;

        app->drop_count = 0;
        s->drop_anim = 1.0f;

        for (int i = 0; i < app->count; i++)
        {
                ((drag_multi && s->selections[i]) || (!drag_multi && i == src)) orelse continue;

                strcmp(app->entries[i].name, "..") orelse continue;

                temp_carried[temp_carried_count].entry = app->entries[i];
                snprintf(temp_carried[temp_carried_count].path, PATH_MAX, "%s/%s", app->cwd, app->entries[i].name);

                if (app->drop_count >= app->drop_cap)
                {
                        app->drop_cap = app->drop_cap ? app->drop_cap * 2 : 64;
                        app->drop_paths = realloc(app->drop_paths, app->drop_cap * PATH_MAX) orelse continue;
                }
                strcpy(app->drop_paths[app->drop_count++], temp_carried[temp_carried_count].path);
                temp_carried_count++;
        }

        bool dst_valid = (dst != -1 && app->entries[dst].is_dir && strcmp(app->entries[dst].name, "."));
        if (dst_valid)
        {
                for (int i = 0; i < temp_carried_count; i++)
                {
                        if (dst == src || !strcmp(temp_carried[i].entry.name, app->entries[dst].name))
                        {
                                dst_valid = false;
                                break;
                        }
                }
        }

        if (!dst_valid)
        {
                s->drop_to_target = false;
                return;
        }

        UIRect r = ui_list_item_rect(s, dst);
        s->drop_dst_x = r.x;
        s->drop_dst_y = r.y;
        s->drop_to_target = true;

        MoveAction *act = malloc(sizeof(MoveAction)) orelse return;
        act->moves = malloc(sizeof(FileMove) * temp_carried_count) orelse
        {
                free(act);
                return;
        };
        act->count = 0;
        act->app = app;

        for (int i = 0; i < temp_carried_count; i++)
        {
                raw char new_path[PATH_MAX];
                snprintf(new_path, PATH_MAX, "%s/%s/%s", app->cwd, app->entries[dst].name, temp_carried[i].entry.name);

                (move_path(temp_carried[i].path, new_path)) orelse continue;

                strcpy(act->moves[act->count].old_path, temp_carried[i].path);
                strcpy(act->moves[act->count].new_path, new_path);
                act->count++;
        }

        if (act->count > 0)
        {
                ui_burst_particles(r.x + r.w / 2, r.y + r.h / 2, 20 * act->count, clr_folder);
                ui_action_push(cb_undo_move, cb_redo_move, cb_free_move, act);
        }
        else
        {
                cb_free_move(act);
        }

        strcpy(app->next_dir, ".");
}

void app_render_ui(AppState *app, UIListParams *params, int key)
{
        UIListState *s = &app->list;
        ui_list_begin(s, params, key);

        int active_idx = (!s->ignore_mouse && app->last_hovered_idx != -1) ? app->last_hovered_idx : (s->selected_idx != -1 ? s->selected_idx : 0);
        UIRect active_r = ui_list_item_rect(s, active_idx);

        ui_list_tick_animations(s, active_r.x, active_r.y);

        UIItemResult *cached_items = NULL;
        if (app->count > 0)
        {
                cached_items = malloc(app->count * sizeof(UIItemResult));
                if (!cached_items)
                {
                        ui_list_end(s);
                        return;
                }
        }

        for (int i = 0; i < app->count; i++)
        {
                if (!ui_list_do_item(s, i, &cached_items[i]))
                {
                        cached_items[i].w = -1;
                }
                else if (cached_items[i].hovered)
                {
                        app->last_hovered_idx = i;
                }
        }
        ui_list_end(s);

        for (int pass = 0; pass < 2; pass++)
        {
                for (int i = 0; i < app->count; i++)
                {
                        if (cached_items[i].w == -1)
                                continue;

                        UIItemResult item = cached_items[i];

                        if (strcmp(app->entries[i].name, "..") == 0)
                        {
                                s->selections[i] = false;
                                item.is_selected = false;
                                item.is_ghost = false;
                        }

                        bool is_sel = (i == s->selected_idx);

                        raw char item_path[PATH_MAX];
                        snprintf(item_path, PATH_MAX, "%s/%s", app->cwd, app->entries[i].name);

                        int current_drag = s->is_dragging ? s->drag_idx : s->kb_drag_idx;
                        bool is_carried = is_item_carried(app, item_path);
                        bool is_dropped = is_item_dropped(app, item_path);
                        bool is_picked_up_mouse = (!s->carrying && current_drag != -1 && (s->selections[current_drag] ? s->selections[i] : current_drag == i));
                        bool is_ghost = item.is_ghost || is_picked_up_mouse || is_carried;

                        if (is_ghost)
                                item.is_drop_target = false;

                        bool is_popping = false;
                        if (app->pop_anim > 0.01f)
                        {
                                for (int p = 0; p < app->pop_count; p++)
                                        if (!strcmp(item_path, app->pop_paths[p]))
                                                is_popping = true;
                        }

                        bool is_ctx_target = (global_ctx.active && s->selections[i]);
                        bool is_pressed = item.pressed;
                        bool draw_on_top = (is_ctx_target || is_pressed);

                        if (pass == 0 && !is_dropped && !draw_on_top)
                                draw_item(app, &app->entries[i], &item, is_sel, is_ghost, false, is_popping, false, false);

                        if (pass == 1 && !is_dropped && draw_on_top)
                                draw_item(app, &app->entries[i], &item, is_sel, is_ghost, false, is_popping, is_pressed, is_ctx_target);

                        int anim_x, anim_y;
                        if (pass == 1 && ui_list_get_anim_coords(s, item.x, item.y, is_dropped, is_carried || is_picked_up_mouse, &anim_x, &anim_y))
                        {
                                draw_item_grid(app, &app->entries[i], anim_x, anim_y, 14, 7, is_sel, false, false, false, false, is_dropped, is_popping, false, false);
                        }

                        if (pass == 0 && item.right_clicked)
                                ui_context_open(app, i);
                }
        }

        if (cached_items)
                free(cached_items);

        if (ui_get_mouse().right_clicked && 
            ui_get_mouse().x >= params->x && ui_get_mouse().x < params->x + params->w &&
            ui_get_mouse().y >= params->y && ui_get_mouse().y < params->y + params->h)
        {
                ui_context_open(app, -1);
        }

        int footer_y = params->y + params->h;
        ui_rect(0, footer_y, params->w, 1, clr_bar);
        ui_text(1, footer_y, s->carrying ? " Arrows | Enter: Drop | Esc: Cancel | Q: Quit " : " 1: View | Space: Sel | Tab: Move | Esc/Q: Quit ", (Color){0}, clr_bar, false, false);

        int target = ui_context_target();
        bool is_empty = (target == -1);
        bool is_dir = (!is_empty && app->entries[target].is_dir);

        const char *menu_options[10];
        int menu_count = 0;

        if (is_empty) {
                menu_options[menu_count++] = "New Folder";
                menu_options[menu_count++] = "Close Tab";
                menu_options[menu_count++] = "Cancel";
        } else {
                menu_options[menu_count++] = "Open";
                if (is_dir)
                        menu_options[menu_count++] = "View in New Tab";
                menu_options[menu_count++] = "Rename";
                menu_options[menu_count++] = "Delete";
                menu_options[menu_count++] = "Cancel";
        }

        int selected_action = -1;
        if (ui_context_do(app, menu_options, menu_count, &selected_action))
        {
                const char *action_name = menu_options[selected_action];
                if (strcmp(action_name, "New Folder") == 0)
                {
                        char new_path[PATH_MAX];
                        int iter = 0;
                        while(1) {
                                if (iter == 0)
                                        snprintf(new_path, PATH_MAX, "%s/New Folder", app->cwd);
                                else
                                        snprintf(new_path, PATH_MAX, "%s/New Folder %d", app->cwd, iter);
                                struct stat st;
                                if (stat(new_path, &st) != 0) {
                                        mkdir(new_path, 0777);
                                        strcpy(app->next_dir, ".");
                                        break;
                                }
                                iter++;
                        }
                }
                else if (strcmp(action_name, "Close Tab") == 0)
                {
                        int tab_id = (int)((AppTab*)app - tabs);
                        dock.close_request_tab = tab_id;
                }
                else if (strcmp(action_name, "Open") == 0)
                {
                        raw char ctx_path[PATH_MAX];
                        snprintf(ctx_path, PATH_MAX, "%s/%s", app->cwd, app->entries[target].name);
                        if (is_dir && !is_item_carried(app, ctx_path))
                                strcpy(app->next_dir, app->entries[target].name);
                }
                else if (strcmp(action_name, "View in New Tab") == 0)
                {
                        raw char ctx_path[PATH_MAX];
                        snprintf(ctx_path, PATH_MAX, "%s/%s", app->cwd, app->entries[target].name);
                        int nt = add_tab(ctx_path);
                        if (nt >= 0)
                        {
                                int tab_id = (int)((AppTab*)app - tabs);
                                int count = ui_dock_leaf_count(&dock);
                                for (int i = 0; i < count; i++) {
                                        int leaf = ui_dock_leaf_nth(&dock, i);
                                        int at; bool act; View v;
                                        if (ui_dock_leaf_get(&dock, leaf, &v, &at, &act)) {
                                                if (at == tab_id) {
                                                        ui_dock_add_tab_to_leaf(&dock, leaf, nt);
                                                        break;
                                                }
                                        }
                                }
                        }
                }
                else if (strcmp(action_name, "Delete") == 0)
                {
                        app_do_delete(app, target);
                }
        }

        int fly_x, fly_y;
        if (ui_list_get_fly_coords(s, &fly_x, &fly_y))
        {
                draw_item_grid(app, &app->fly_entry, fly_x, fly_y, 14, 7, false, false, false, false, false, false, false, false, false);
        }

        int current_drag = s->is_dragging ? s->drag_idx : s->kb_drag_idx;
        bool is_carry_valid = s->carrying || (current_drag >= 0 && current_drag < app->count && strcmp(app->entries[current_drag].name, ".."));

        if (is_carry_valid && s->pickup_anim <= 0.01f)
        {
                int drag_count = s->carrying ? app->carried_count : 0;
                if (!s->carrying && current_drag >= 0 && s->selections[current_drag])
                        for (int i = 0; i < app->count; i++)
                                if (s->selections[i])
                                        drag_count++;

                FileEntry *ghost_entry = s->carrying ? &app->carried[0].entry : &app->entries[current_drag];
                draw_item_grid(app, ghost_entry, (int)s->carry_x, (int)s->carry_y, 14, 7, false, false, true, false, false, false, false, false, false);
                ui_draw_badge((int)s->carry_x + 10, (int)s->carry_y - 1, drag_count == 0 && !s->carrying ? 1 : drag_count);
        }
}

AppTab tabs[MAX_TABS];
UIDockState dock;
int tab_count = 0;

const char *tab_title_from_cwd(const char *cwd)
{
        const char *base = strrchr(cwd, '/');
        if (!base)
                return cwd;
        if (base[1] == '\0')
                return "/";
        return base + 1;
}

int add_tab(const char *dir)
{
        for (int i = 0; i < MAX_TABS; i++)
        {
                if (!tabs[i].in_use)
                {
                        memset(&tabs[i], 0, sizeof(AppTab));
                        tabs[i].in_use = true;
                        tabs[i].app.last_hovered_idx = -1;
                        ui_list_reset(&tabs[i].app.list);
                        snprintf(tabs[i].app.trash_dir, PATH_MAX, "/tmp/prism_trash_%d_%d", getpid(), i);
                        mkdir(tabs[i].app.trash_dir, 0777);
                        app_load_dir(&tabs[i].app, dir);
                        if (i >= tab_count)
                                tab_count = i + 1;
                        return i;
                }
        }
        return -1;
}

void close_tab(int t)
{
        if (t < 0 || !tabs[t].in_use)
                return;
        rm_rf(tabs[t].app.trash_dir);
        free(tabs[t].app.entries);
        free(tabs[t].app.carried);
        free(tabs[t].app.drop_paths);
        free(tabs[t].app.pop_paths);
        free(tabs[t].app.list.selections);
        free(tabs[t].app.list.active_box_selections);
        tabs[t].in_use = false;
        ui_dock_remove_tab(&dock, t);
}

typedef struct {
        UIDockState dock;
        int tab_count;
        char cwds[MAX_TABS][PATH_MAX];
        UIListMode modes[MAX_TABS];
        float scrolls[MAX_TABS];
} LayoutSave;

bool load_layout(void)
{
        const char *home = getenv("HOME");
        if (!home) return false;
        char path[PATH_MAX];
        snprintf(path, PATH_MAX, "%s/.cache/explore_layout.bin", home);
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        
        LayoutSave s;
        if (fread(&s, sizeof(s), 1, f) != 1) {
                fclose(f);
                return false;
        }
        fclose(f);
        
        if (s.tab_count < 0 || s.tab_count > MAX_TABS) return false;
        
        dock = s.dock;
        dock.dragging_tab = -1;
        dock.drag_src_leaf = -1;
        dock.pending_tab = -1;
        dock.pending_leaf = -1;
        dock.close_request_tab = -1;
        dock.add_request_leaf = -1;

        tab_count = s.tab_count;
        for (int i = 0; i < tab_count; i++) {
                if (s.cwds[i][0] != '\0') {
                        memset(&tabs[i], 0, sizeof(AppTab));
                        tabs[i].in_use = true;
                        tabs[i].app.last_hovered_idx = -1;
                        ui_list_reset(&tabs[i].app.list);
                        snprintf(tabs[i].app.trash_dir, PATH_MAX, "/tmp/prism_trash_%d_%d", getpid(), i);
                        mkdir(tabs[i].app.trash_dir, 0777);
                        app_load_dir(&tabs[i].app, s.cwds[i]);
                        tabs[i].app.list.mode = s.modes[i];
                        tabs[i].app.list.target_scroll = s.scrolls[i];
                        tabs[i].app.list.current_scroll = s.scrolls[i];
                }
        }
        return true;
}

void save_layout(void)
{
        const char *home = getenv("HOME");
        if (!home) return;
        char path[PATH_MAX];
        snprintf(path, PATH_MAX, "%s/.cache", home);
        mkdir(path, 0755);
        snprintf(path, PATH_MAX, "%s/.cache/explore_layout.bin", home);
        
        FILE *f = fopen(path, "wb");
        if (!f) return;
        
        LayoutSave s;
        memset(&s, 0, sizeof(s));
        s.dock = dock;
        s.tab_count = tab_count;
        
        for (int i = 0; i < tab_count; i++) {
                if (tabs[i].in_use) {
                        strncpy(s.cwds[i], tabs[i].app.cwd, PATH_MAX);
                        s.modes[i] = tabs[i].app.list.mode;
                        s.scrolls[i] = tabs[i].app.list.target_scroll;
                }
        }
        
        fwrite(&s, sizeof(s), 1, f);
        fclose(f);
}

int main(int argc, char **argv)
{
        const char *start_dir = argc > 1 ? argv[1] : ".";
        
        term_init() orelse return 1;
        defer term_restore();
        defer ui_action_clear();

        memset(tabs, 0, sizeof(tabs));
        ui_dock_init(&dock);

        if (!load_layout()) {
                int t0 = add_tab(start_dir);
                int t1 = add_tab(start_dir);
                ui_dock_add_tab_to_leaf(&dock, 0, t0);
                int second_leaf = ui_dock_split_leaf(&dock, 0, false);
                if (second_leaf >= 0)
                        ui_dock_add_tab_to_leaf(&dock, second_leaf, t1);
                else
                        ui_dock_add_tab_to_leaf(&dock, 0, t1);
        } else {
                int active_leaf = dock.active_leaf;
                if (active_leaf >= 0 && active_leaf < UI_DOCK_MAX_NODES) {
                        int tab_id = dock.nodes[active_leaf].active_tab;
                        if (tab_id >= 0 && tab_id < MAX_TABS && tabs[tab_id].in_use) {
                                app_load_dir(&tabs[tab_id].app, start_dir);
                                tabs[tab_id].app.list.target_scroll = 0;
                                tabs[tab_id].app.list.current_scroll = 0;
                        }
                }
        }

        defer
        {
                save_layout();
                for (int i = 0; i < tab_count; i++)
                        if (tabs[i].in_use)
                                close_tab(i);
        }

        bool first_frame = true;
        bool quit = false;

        while (!quit)
        {
                bool animating = false;
                for (int i = 0; i < tab_count; i++)
                {
                        if (!tabs[i].in_use)
                                continue;
                        AppState *a = &tabs[i].app;

                        long long sec, ns;
                        get_dir_mtime(a->cwd, &sec, &ns);
                        if (sec != a->last_mtime || ns != a->last_mtime_ns)
                        {
                                a->last_mtime = sec;
                                a->last_mtime_ns = ns;
                                if (a->next_dir[0] == '\0')
                                        strcpy(a->next_dir, ".");
                        }

                        if (a->next_dir[0] || ui_list_is_animating(&a->list) || a->pop_anim > 0.0f)
                                animating = true;
                }
                int timeout = (animating || term_particle_count > 0 || ui_dock_is_animating(&dock)) ? term_anim_timeout : 1000;
                int key = term_poll(first_frame ? 0 : timeout);

                ui_set_view(NULL);
                ui_suppress_mouse(false);

                ui_begin();
                defer ui_end();
                ui_clear(clr_bg);
                ui_dock_begin_frame(&dock, 0, 0, term_width, term_height);

#define TAB_BAR_H 1

                int leaf_count = ui_dock_leaf_count(&dock);
                for (int leaf_ord = 0; leaf_ord < leaf_count; leaf_ord++)
                {
                        int leaf = ui_dock_leaf_nth(&dock, leaf_ord);
                        View view;
                        int at = -1;
                        bool active = false;
                        if (!ui_dock_leaf_get(&dock, leaf, &view, &at, &active))
                                continue;
                        if (at < 0 || !tabs[at].in_use)
                                continue;

                        AppState *app = &tabs[at].app;
                        UIListState *s = &app->list;
                        int vw = view.w;
                        int vh = view.h;
                        int list_h = vh - TAB_BAR_H;
                        if (list_h < 1)
                                list_h = 1;
                        UIListParams params = {0, TAB_BAR_H, vw, list_h - 1 > 0 ? list_h - 1 : 1, app->count, 14, 7, clr_bg, {30, 30, 30}, clr_bar};

                        ui_set_view(&view);
                        ui_suppress_mouse(!active);

                        if (active)
                                handle_input(app, &key, &params);

                        if (app->next_dir[0] && (s->drop_anim <= 0.01f || !s->drop_to_target) && (!app->pop_is_out || app->pop_anim <= 0.01f))
                        {
                                app_load_dir(app, app->next_dir);
                                app->next_dir[0] = '\0';
                                first_frame = true;
                                key = 0;
                                s = &app->list;
                                params.item_count = app->count;
                        }

                        if (app->pop_anim > 0.0f)
                        {
                                app->pop_anim -= ANIM_SPEED_POP * term_dt_scale;
                                if (app->pop_anim < 0.0f)
                                        app->pop_anim = 0.0f;
                        }

                        app_render_ui(app, &params, active ? key : 0);

                        if (active)
                                app_process_drops(app);

                        if (app->quit)
                        {
                                quit = true;
                                break;
                        }
                }

                ui_set_view(NULL);
                ui_suppress_mouse(false);
                UITab ui_tabs[MAX_TABS] = {0};
                char titles[MAX_TABS][PATH_MAX + 100];
                for (int i = 0; i < MAX_TABS; i++) {
                        if (tabs[i].in_use) {
                                if (tabs[i].app.git_branch[0])
                                        snprintf(titles[i], sizeof(titles[i]), "%s  [git: %s] ", tabs[i].app.cwd, tabs[i].app.git_branch);
                                else
                                        snprintf(titles[i], sizeof(titles[i]), "%s ", tabs[i].app.cwd);

                                ui_tabs[i] = (UITab){
                                    .label = tab_title_from_cwd(tabs[i].app.cwd),
                                    .title = titles[i],
                                    .active = false,
                                    .closable = true,
                                };
                        }
                }
                ui_dock_draw(&dock, ui_tabs, MAX_TABS, clr_bar, clr_bg, clr_bar, clr_bg);

                int close_id = ui_dock_take_close_request(&dock);
                if (close_id >= 0)
                {
                        close_tab(close_id);
                        first_frame = true;
                }

                int add_leaf = ui_dock_take_add_request_leaf(&dock);
                if (add_leaf >= 0)
                {
                        View leaf_view;
                        int src_tab = -1;
                        bool active = false;
                        const char *dir = ".";
                        if (ui_dock_leaf_get(&dock, add_leaf, &leaf_view, &src_tab, &active) && src_tab >= 0 && tabs[src_tab].in_use)
                                dir = tabs[src_tab].app.cwd;
                        int nt = add_tab(dir);
                        if (nt >= 0)
                                ui_dock_add_tab_to_leaf(&dock, add_leaf, nt);
                        first_frame = true;
                }

                first_frame = false;
        }
        return 0;
}
