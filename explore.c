#include "terminal.c"
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <ctype.h>

Color clr_bg, clr_bar = {170, 170, 170}, clr_text = {255, 255, 255}, clr_folder = {255, 255, 85}, clr_hover = {170, 170, 170}, clr_sel_bg = {40, 70, 120};

typedef struct AppState AppState;

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
} FileEntry;

typedef struct
{
        FileEntry entry;
        char path[PATH_MAX];
} CarriedFile;

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
};

void cb_undo_move(void *data)
{
        MoveAction *act = (MoveAction *)data;
        act->app->pop_count = 0;
        act->app->pop_anim = 1.0f;
        act->app->pop_is_out = false;

        for (int i = 0; i < act->count; i++)
        {
                rename(act->moves[i].new_path, act->moves[i].old_path);

                if (act->app->pop_count >= act->app->pop_cap)
                {
                        act->app->pop_cap = act->app->pop_cap ? act->app->pop_cap * 2 : 64;
                        act->app->pop_paths = realloc(act->app->pop_paths, act->app->pop_cap * PATH_MAX) orelse return;
                }
                strcpy(act->app->pop_paths[act->app->pop_count++], act->moves[i].old_path);
        }
}

void cb_redo_move(void *data)
{
        MoveAction *act = (MoveAction *)data;
        act->app->pop_count = 0;
        act->app->pop_anim = 1.0f;
        act->app->pop_is_out = true;

        for (int i = 0; i < act->count; i++)
        {
                rename(act->moves[i].old_path, act->moves[i].new_path);

                if (act->app->pop_count >= act->app->pop_cap)
                {
                        act->app->pop_cap = act->app->pop_cap ? act->app->pop_cap * 2 : 64;
                        act->app->pop_paths = realloc(act->app->pop_paths, act->app->pop_cap * PATH_MAX) orelse return;
                }
                strcpy(act->app->pop_paths[act->app->pop_count++], act->moves[i].new_path);
        }
}

void cb_free_move(void *data)
{
        MoveAction *act = (MoveAction *)data;
        free(act->moves);
        free(act);
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

void draw_item_grid(AppState *app, FileEntry *e, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target, bool is_multi_sel, bool is_dropped, bool is_popping)
{
        int float_y = 0;
        if (is_popping)
                float_y = app->pop_is_out ? (int)((1.0f - app->pop_anim) * 2.0f) : -(int)(app->pop_anim * 2.0f);
        y += float_y;

        Color item_bg = (is_drop_target && e->is_dir) ? (Color){50, 150, 50} : (is_ghost ? (Color){30, 30, 30} : (is_multi_sel ? clr_sel_bg : (is_hover || is_sel ? clr_hover : clr_bg)));

        if (is_popping)
        {
                int flash = (int)(app->pop_anim * 80.0f);
                item_bg.r = item_bg.r + flash > 255 ? 255 : item_bg.r + flash;
                item_bg.g = item_bg.g + flash > 255 ? 255 : item_bg.g + flash;
                item_bg.b = item_bg.b + flash > 255 ? 255 : item_bg.b + flash;
        }

        Color icon_fg = is_ghost ? (Color){100, 100, 100} : (e->is_dir ? clr_folder : (e->is_exec ? (Color){85, 255, 85} : clr_text));

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

        int y_off = (is_hover && term_mouse.left && !app->list.is_dragging) ? 1 : 0;
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
        ui_text_centered(x + 1, y + 4 + y_off, mw, l1, is_ghost ? icon_fg : clr_text, item_bg, is_dropped, false);
        if (l2[0])
                ui_text_centered(x + 1, y + 5 + y_off, mw, l2, is_ghost ? icon_fg : clr_text, item_bg, is_dropped, false);

        if (is_popping)
        {
                float scale = app->pop_is_out ? app->pop_anim : (1.0f - app->pop_anim);
                ui_scale_region(x, y, w, h, scale, clr_bg);
        }
}

void draw_item_list(AppState *app, FileEntry *e, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target, bool is_multi_sel, bool is_dropped, bool is_popping)
{
        int float_y = 0;
        if (is_popping)
                float_y = app->pop_is_out ? (int)((1.0f - app->pop_anim) * 1.0f) : -(int)(app->pop_anim * 1.0f);
        y += float_y;

        Color item_bg = (is_drop_target && e->is_dir) ? (Color){50, 150, 50} : (is_ghost ? (Color){30, 30, 30} : (is_multi_sel ? clr_sel_bg : (is_hover || is_sel ? clr_hover : clr_bg)));

        if (is_popping)
        {
                int flash = (int)(app->pop_anim * 80.0f);
                item_bg.r = item_bg.r + flash > 255 ? 255 : item_bg.r + flash;
                item_bg.g = item_bg.g + flash > 255 ? 255 : item_bg.g + flash;
                item_bg.b = item_bg.b + flash > 255 ? 255 : item_bg.b + flash;
        }

        Color icon_fg = is_ghost ? (Color){100, 100, 100} : (e->is_dir ? clr_folder : (e->is_exec ? (Color){85, 255, 85} : clr_text));

        ui_rect(x, y, w, 1, item_bg);

        ui_text(x + 1, y, e->is_dir ? " ▓]" : " ■", icon_fg, item_bg, false, false);

        raw char l[256];
        int copy_len = w - 9 > 255 ? 255 : (w - 9 < 0 ? 0 : w - 9);
        strncpy(l, e->name, copy_len);
        l[copy_len] = '\0';

        ui_text(x + 8, y, l, is_ghost ? icon_fg : clr_text, item_bg, is_dropped, false);

        raw char size_str[32];
        if (!e->is_dir)
        {
                off_t s = e->size;
                if (s < 1024)
                        snprintf(size_str, 32, "%4lld B ", (long long)s);
                else if (s < 1024 * 1024)
                        snprintf(size_str, 32, "%4lld KB", (long long)(s >> 10));
                else if (s < 1024 * 1024 * 1024)
                        snprintf(size_str, 32, "%4lld MB", (long long)(s >> 20));
                else
                        snprintf(size_str, 32, "%4lld GB", (long long)(s >> 30));
                ui_text(x + w - 10, y, size_str, is_ghost ? icon_fg : clr_bar, item_bg, false, false);
        }

        if (is_popping)
        {
                float scale = app->pop_is_out ? app->pop_anim : (1.0f - app->pop_anim);
                ui_scale_region(x, y, w, h, scale, clr_bg);
        }
}

void draw_item(AppState *app, FileEntry *e, UIItemResult *res, bool is_sel, bool is_ghost, bool is_dropped, bool is_popping)
{
        if (app->list.mode == UI_MODE_GRID)
                draw_item_grid(app, e, res->x, res->y, res->w, res->h, is_sel, res->hovered, is_ghost, res->is_drop_target, res->is_selected, is_dropped, is_popping);
        else
                draw_item_list(app, e, res->x, res->y, res->w, res->h, is_sel, res->hovered, is_ghost, res->is_drop_target, res->is_selected, is_dropped, is_popping);
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

        DIR *d = opendir(path) orelse opendir(".");
        d orelse return;
        defer closedir(d);
        (chdir(path) == 0) orelse return;
        getcwd(app->cwd, sizeof(app->cwd)) orelse return;

        char sel[256];
        if (app->count > 0 && app->list.selected_idx >= 0)
                strcpy(sel, app->entries[app->list.selected_idx].name);

        UIListMode m = app->list.mode;
        ui_list_reset(&app->list);
        app->list.mode = m;
        app->count = 0;

        while (1)
        {
                struct dirent *dir = readdir(d) orelse break;
                if (!strcmp(dir->d_name, "."))
                        continue;

                if (app->count >= app->capacity)
                {
                        app->capacity = app->capacity ? app->capacity * 2 : 256;
                        app->entries = realloc(app->entries, app->capacity * sizeof(FileEntry)) orelse break;
                }

                raw struct stat st;
                (!stat(dir->d_name, &st)) orelse continue;

                FileEntry *e = &app->entries[app->count++];
                strcpy(e->name, dir->d_name);
                e->size = st.st_size;
                e->is_dir = S_ISDIR(st.st_mode);
                e->is_exec = (st.st_mode & S_IXUSR) && !e->is_dir;
        }
        qsort(app->entries, app->count, sizeof(FileEntry), cmp_entries);

        if (dir_changed && app->count > 0)
        {
                app->list.selected_idx = 0;
        }
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
}

void handle_input(AppState *app, int *key, const UIListParams *params)
{
        UIListState *s = &app->list;

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
        if (*key == 'v')
                ui_list_set_mode(s, params, !s->mode);
        if (*key == KEY_BACKSPACE)
                strcpy(app->next_dir, "..");

        if (*key == KEY_ESC)
        {
                s->carrying = false;
                ui_list_clear_selections(s);
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

                                (rename(app->carried[i].path, new_path) == 0) orelse continue;

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

                (rename(temp_carried[i].path, new_path) == 0) orelse continue;

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

        int active_idx = s->selected_idx != -1 ? s->selected_idx : (app->last_hovered_idx == -1 ? 0 : app->last_hovered_idx);
        UIRect active_r = ui_list_item_rect(s, active_idx);

        ui_list_tick_animations(s, active_r.x, active_r.y);

        for (int pass = 0; pass < 2; pass++)
        {
                for (int i = 0; i < app->count; i++)
                {
                        UIItemResult item;
                        UIRect item_r = ui_list_item_rect(s, i);

                        if (pass == 0)
                        {
                                ui_list_do_item(s, i, &item) orelse continue;
                                if (item.hovered)
                                        app->last_hovered_idx = i;
                        }
                        else
                        {
                                item = (UIItemResult){.x = item_r.x, .y = item_r.y, .w = item_r.w, .h = item_r.h};
                        }

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

                        if (pass == 0 && !is_dropped)
                                draw_item(app, &app->entries[i], &item, is_sel, is_ghost, false, is_popping);

                        int anim_x, anim_y;
                        if (pass == 1 && ui_list_get_anim_coords(s, item.x, item.y, is_dropped, is_carried || is_picked_up_mouse, &anim_x, &anim_y))
                        {
                                draw_item_grid(app, &app->entries[i], anim_x, anim_y, 14, 7, is_sel, false, false, false, false, is_dropped, is_popping);
                        }

                        if (pass == 0 && item.right_clicked)
                                ui_context_open(i);
                }
                if (pass == 0)
                        ui_list_end(s);
        }

        ui_rect(0, 0, term_width, params->y, clr_bg);
        ui_rect(0, 0, term_width, 1, clr_bar);
        raw char header[PATH_MAX + 20];
        snprintf(header, sizeof(header), "%s ", app->cwd);
        ui_text(1, 0, header, (Color){0}, clr_bar, false, false);

        ui_rect(0, term_height - 1, term_width, 1, clr_bar);
        ui_text(1, term_height - 1, s->carrying ? " Arrows: Navigate | Enter: Drop | 'Tab': Drop Here | Esc: Cancel " : " Arrows: Navigate | Space: Select | Tab: Move | u: Undo | r: Redo | Esc: Cancel ", (Color){0}, clr_bar, false, false);

        const char *menu_options[] = {"Open", "Rename", "Delete", "Cancel"};
        int selected_action = -1;
        if (ui_context_do(menu_options, 4, &selected_action) && selected_action == 0)
        {
                raw char ctx_path[PATH_MAX];
                snprintf(ctx_path, PATH_MAX, "%s/%s", app->cwd, app->entries[ui_context_target()].name);
                if (app->entries[ui_context_target()].is_dir && !is_item_carried(app, ctx_path))
                        strcpy(app->next_dir, app->entries[ui_context_target()].name);
        }

        int fly_x, fly_y;
        if (ui_list_get_fly_coords(s, &fly_x, &fly_y))
        {
                draw_item_grid(app, &app->fly_entry, fly_x, fly_y, 14, 7, false, false, false, false, false, false, false);
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
                draw_item_grid(app, ghost_entry, (int)s->carry_x, (int)s->carry_y, 14, 7, false, false, true, false, false, false, false);
                ui_draw_badge((int)s->carry_x + 10, (int)s->carry_y - 1, drag_count == 0 && !s->carrying ? 1 : drag_count);
        }
}

int main(void)
{
        term_init() orelse return 1;
        defer term_restore();
        defer ui_action_clear();

        AppState app;
        app.last_hovered_idx = -1;
        defer free(app.entries);
        defer free(app.carried);
        defer free(app.drop_paths);
        defer free(app.pop_paths);

        app_load_dir(&app, ".");

        bool first_frame = true, was_carrying, was_dragging;

        while (!app.quit)
        {
                UIListState *s = &app.list;
                int timeout = (app.next_dir[0] || ui_list_is_animating(s) || app.pop_anim > 0.0f) ? term_anim_timeout : 1000;

                int key = term_poll(first_frame ? 0 : timeout);
                UIListParams params = {0, 1, term_width, term_height - 2 > 0 ? term_height - 2 : 1, app.count, 14, 7, clr_bg, {30, 30, 30}, clr_bar};

                handle_input(&app, &key, &params);

                if (app.next_dir[0] && (s->drop_anim <= 0.01f || !s->drop_to_target) && (!app.pop_is_out || app.pop_anim <= 0.01f))
                {
                        app_load_dir(&app, app.next_dir);
                        app.next_dir[0] = '\0';
                        first_frame = true;
                        continue;
                }

                if (app.pop_anim > 0.0f)
                {
                        app.pop_anim -= ANIM_SPEED_POP * term_dt_scale;
                        if (app.pop_anim < 0.0f)
                                app.pop_anim = 0.0f;
                }

                ui_begin();
                defer ui_end();
                ui_clear(clr_bg);

                if (s->carrying && !was_carrying)
                {
                        int c_idx = s->selected_idx != -1 ? s->selected_idx : (app.last_hovered_idx != -1 ? app.last_hovered_idx : 0);
                        s->carry_x = ui_list_item_rect(s, c_idx).x;
                        s->carry_y = ui_list_item_rect(s, c_idx).y;
                }

                if (s->is_dragging && !was_dragging)
                {
                        bool drag_multi = s->drag_idx >= 0 && s->selections[s->drag_idx];
                        for (int i = 0; i < app.count; i++)
                        {
                                if ((drag_multi && s->selections[i]) || (!drag_multi && i == s->drag_idx))
                                {
                                        if (strcmp(app.entries[i].name, "..") != 0)
                                        {
                                                UIRect br = ui_list_item_rect(s, i);
                                                ui_burst_particles(br.x + br.w / 2, br.y + br.h / 2, drag_multi ? 5 : 15, clr_text);
                                        }
                                }
                        }
                }

                app_render_ui(&app, &params, key);
                app_process_drops(&app);

                was_carrying = s->carrying;
                was_dragging = s->is_dragging;
                first_frame = false;
        }
        return 0;
}