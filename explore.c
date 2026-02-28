#include "terminal.c"
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <ctype.h>

float anim_speed_carry = 0.4f;
float anim_speed_drop = 0.10f;
float anim_speed_fly = 0.15f;

Color clr_bg, clr_bar = {170, 170, 170}, clr_text = {255, 255, 255}, clr_folder = {255, 255, 85}, clr_hover = {170, 170, 170}, clr_sel_bg = {40, 70, 120};

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

typedef struct
{
        FileEntry *entries;
        int capacity, count;
        char cwd[PATH_MAX], next_dir[PATH_MAX];
        UIListState list;
        bool quit;

        CarriedFile carried[256];
        int carried_count;

        char drop_names[64][256];
        int drop_count;

        int last_hovered_idx;
        FileEntry fly_entry;
} AppState;

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

void draw_item_grid(AppState *app, FileEntry *e, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target, bool is_multi_sel, bool is_dropped)
{
        Color item_bg = is_drop_target ? (Color){50, 150, 50} : (is_ghost ? (Color){30, 30, 30} : (is_multi_sel ? clr_sel_bg : (is_hover || is_sel ? clr_hover : clr_bg)));
        Color icon_fg = is_ghost ? (Color){100, 100, 100} : (e->is_dir ? clr_folder : (e->is_exec ? (Color){85, 255, 85} : clr_text));

        if (is_hover || is_sel || is_ghost || is_drop_target || is_multi_sel)
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

        char l1[16], l2[16];
        int mw = w - 2 > 15 ? 15 : (w - 2 < 0 ? 0 : w - 2);
        int len = strlen(e->name);

        strncpy(l1, e->name, mw);
        if (len > mw)
        {
                strncpy(l2, e->name + mw, mw);
                if (len > mw * 2)
                {
                        l2[mw - 2] = '.';
                        l2[mw - 1] = '.';
                }
        }
        ui_text_centered(x + 1, y + 4 + y_off, mw, l1, is_ghost ? icon_fg : clr_text, item_bg, is_dropped, false);
        if (l2[0])
                ui_text_centered(x + 1, y + 5 + y_off, mw, l2, is_ghost ? icon_fg : clr_text, item_bg, is_dropped, false);
}

void draw_item_list(AppState *app, FileEntry *e, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target, bool is_multi_sel, bool is_dropped)
{
        Color item_bg = is_drop_target ? (Color){50, 150, 50} : (is_ghost ? (Color){30, 30, 30} : (is_multi_sel ? clr_sel_bg : (is_hover || is_sel ? clr_hover : clr_bg)));
        Color icon_fg = is_ghost ? (Color){100, 100, 100} : (e->is_dir ? clr_folder : (e->is_exec ? (Color){85, 255, 85} : clr_text));

        ui_rect(x, y, w, 1, item_bg);
        ui_text(x + 1, y, e->is_dir ? " ▓]" : " ■", icon_fg, item_bg, false, false);

        char l[256];
        int copy_len = w - 9 > 255 ? 255 : (w - 9 < 0 ? 0 : w - 9);
        strncpy(l, e->name, copy_len);

        ui_text(x + 8, y, l, is_ghost ? icon_fg : clr_text, item_bg, is_dropped, false);

        char size_str[32];
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
}

bool is_item_dropped(AppState *app, const char *name)
{
        if (app->list.drop_anim <= 0.01f)
                return false;
        for (int d = 0; d < app->drop_count; d++)
                if (!strcmp(name, app->drop_names[d]))
                        return true;
        return false;
}

bool is_item_carried(AppState *app, const char *name)
{
        if (!app->list.carrying)
                return false;
        for (int c = 0; c < app->carried_count; c++)
                if (!strcmp(name, app->carried[c].entry.name))
                        return true;
        return false;
}

void draw_item(AppState *app, FileEntry *e, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target, bool is_multi_sel, bool is_dropped)
{
        if (app->list.mode == UI_MODE_GRID)
                draw_item_grid(app, e, x, y, w, h, is_sel, is_hover, is_ghost, is_drop_target, is_multi_sel, is_dropped);
        else
                draw_item_list(app, e, x, y, w, h, is_sel, is_hover, is_ghost, is_drop_target, is_multi_sel, is_dropped);
}

void app_load_dir(AppState *app, const char *path)
{
        DIR *d = opendir(path) orelse
        {
                d = opendir(".");
                strcpy(app->cwd, ".");
        };
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
                !stat(dir->d_name, &st) orelse continue;

                FileEntry *e = &app->entries[app->count++];
                strcpy(e->name, dir->d_name);
                e->size = st.st_size;
                e->is_dir = S_ISDIR(st.st_mode);
                e->is_exec = (st.st_mode & S_IXUSR) && !e->is_dir;
        }
        qsort(app->entries, app->count, sizeof(FileEntry), cmp_entries);

        if (app->count > 0 && sel[0])
                for (int i = 0; i < app->count; i++)
                        if (!strcmp(app->entries[i].name, sel))
                        {
                                app->list.selected_idx = i;
                                break;
                        }
}
void handle_input(AppState *app, int *key, const UIListParams *params)
{
        UIListState *s = &app->list;
        if (*key == 'q' && !s->carrying)
                app->quit = true;
        if (*key == 'v')
                ui_list_set_mode(s, params, !s->mode);
        if (*key == KEY_BACKSPACE) // Unblocked: allow backing out while carrying files!
                strcpy(app->next_dir, "..");
        if (*key == KEY_ESC)
        {
                s->carrying = false;
                ui_list_clear_selections(s);
        }

        // Reverted: Pure navigation. The files will keep following you!
        if (*key == KEY_ENTER && app->count > 0 && s->selected_idx >= 0 && app->entries[s->selected_idx].is_dir)
        {
                strcpy(app->next_dir, app->entries[s->selected_idx].name);
                *key = 0;
        }

        if (*key == ' ' && s->carrying)
        {
                int src = s->selected_idx != -1 ? s->selected_idx : app->last_hovered_idx;
                if (src >= 0 && src < app->count && strcmp(app->entries[src].name, "..") != 0)
                {
                        int found_idx = -1;
                        for (int i = 0; i < app->carried_count; i++)
                                if (!strcmp(app->carried[i].entry.name, app->entries[src].name))
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
                        else if (app->carried_count < 256)
                        {
                                s->fly_is_pickup = true;
                                app->carried[app->carried_count].entry = app->entries[src];
                                snprintf(app->carried[app->carried_count++].path, PATH_MAX, "%s/%s", app->cwd, app->entries[src].name);
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
                        for (int i = 0; i < app->carried_count; i++)
                        {
                                char new_path[PATH_MAX];
                                snprintf(new_path, PATH_MAX, "%s/%s", app->cwd, app->carried[i].entry.name);
                                rename(app->carried[i].path, new_path);
                                if (app->drop_count < 64)
                                        strcpy(app->drop_names[app->drop_count++], app->carried[i].entry.name);
                        }
                        s->carrying = false;
                        strcpy(app->next_dir, ".");
                }
                else
                {
                        app->carried_count = 0;
                        bool drag_multi;
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
                                                app->carried[app->carried_count].entry = app->entries[i];
                                                snprintf(app->carried[app->carried_count++].path, PATH_MAX, "%s/%s", app->cwd, app->entries[i].name);
                                        }
                                }
                        }
                        if (app->carried_count > 0)
                        {
                                s->carrying = true;
                                s->pickup_anim = 1.0f;
                        }
                }
                *key = 0;
        }
}

int main(void)
{
        term_init() orelse return 1;
        defer term_restore();

        AppState app;
        app.last_hovered_idx = -1;
        defer free(app.entries);
        app_load_dir(&app, ".");

        bool first_frame = true, was_carrying, was_dragging;

        while (!app.quit)
        {
                UIListState *s = &app.list;
                int timeout = (app.next_dir[0] || ui_list_is_animating(s) || s->is_dragging || s->is_kb_dragging || s->is_box_selecting || s->carrying || s->drop_anim > 0.0f || s->fly_anim > 0.0f || s->pickup_anim > 0.0f) ? term_anim_timeout : 1000;
                if (first_frame)
                        timeout = 0;

                int key = term_poll(timeout);
                UIListParams params = {0, 1, term_width, term_height - 2 > 0 ? term_height - 2 : 1, app.count, 14, 7, clr_bg, {30, 30, 30}, clr_bar};
                handle_input(&app, &key, &params);

                // Do not wait for drop_anim if we are dropping into the current grid (!drop_to_target)
                if (app.next_dir[0] && (s->drop_anim <= 0.01f || !s->drop_to_target))
                {
                        app_load_dir(&app, app.next_dir);
                        app.next_dir[0] = '\0';
                        first_frame = true;
                        continue;
                }

                ui_begin();
                ui_clear(clr_bg);
                ui_list_begin(s, &params, key);

                int active_idx = s->selected_idx != -1 ? s->selected_idx : (app.last_hovered_idx == -1 ? 0 : app.last_hovered_idx);
                UIRect active_r = ui_list_item_rect(s, active_idx);

                if (s->carrying && !was_carrying)
                {
                        s->carry_x = active_r.x;
                        s->carry_y = active_r.y;
                }
                ui_list_tick_animations(s, term_mouse, term_dt_scale, active_r.x, active_r.y, anim_speed_carry, anim_speed_fly, anim_speed_drop);

                for (int pass = 0; pass < 2; pass++)
                {
                        for (int i = 0; i < app.count; i++)
                        {
                                UIItemResult item;
                                UIRect item_r = ui_list_item_rect(s, i);

                                if (pass == 0)
                                {
                                        ui_list_do_item(s, i, &item) orelse continue;
                                        if (item.hovered)
                                                app.last_hovered_idx = i;
                                }
                                else
                                        item = (UIItemResult){.x = item_r.x, .y = item_r.y, .w = item_r.w, .h = item_r.h};

                                if (!strcmp(app.entries[i].name, ".."))
                                {
                                        s->selections[i] = false;
                                        item.is_selected = false;
                                        item.is_ghost = false;
                                }

                                int current_drag = s->is_dragging ? s->drag_idx : s->kb_drag_idx;
                                bool is_sel = (i == s->selected_idx);
                                bool valid_drop = item.is_drop_target && app.entries[i].is_dir;

                                bool is_carried = is_item_carried(&app, app.entries[i].name);
                                bool is_dropped = is_item_dropped(&app, app.entries[i].name);
                                bool is_picked_up_mouse = (!s->carrying && current_drag != -1 && (s->selections[current_drag] ? item.is_selected : current_drag == i));
                                bool is_ghost = item.is_ghost || is_picked_up_mouse || is_carried;

                                if (pass == 0 && !is_dropped)
                                        draw_item(&app, &app.entries[i], item.x, item.y, item.w, item.h, is_sel, item.hovered, is_ghost, valid_drop, item.is_selected, false);

                                if (pass == 1 && (is_dropped || ((is_carried || is_picked_up_mouse) && s->pickup_anim > 0.01f)))
                                {
                                        float e = is_dropped ? s->drop_anim * s->drop_anim : s->pickup_anim * s->pickup_anim;
                                        int r_x = is_dropped ? (s->drop_to_target ? s->drop_dst_x + (s->carry_x - s->drop_dst_x) * e : item.x + (s->carry_x - item.x) * e) : s->carry_x + (item.x - s->carry_x) * e;
                                        int r_y = is_dropped ? (s->drop_to_target ? s->drop_dst_y + (s->carry_y - s->drop_dst_y) * e : item.y + (s->carry_y - item.y) * e) : s->carry_y + (item.y - s->carry_y) * e;
                                        draw_item(&app, &app.entries[i], r_x, r_y, item_r.w, item_r.h, false, false, false, false, false, is_dropped);
                                }
                                if (pass == 0 && item.right_clicked)
                                        ui_context_open(i);
                        }
                        if (pass == 0)
                                ui_list_end(s);
                }

                if (s->action_click_idx != -1 && app.entries[s->action_click_idx].is_dir)
                        strcpy(app.next_dir, app.entries[s->action_click_idx].name);
                if (s->action_drop_src != -1)
                {
                        int src = s->action_drop_src, dst = s->action_drop_dst;
                        bool drag_multi;
                        if (src >= 0 && s->selections[src])
                                for (int i = 0; i < app.count; i++)
                                        if (s->selections[i])
                                                drag_multi = true;

                        int temp_carried_count;
                        raw CarriedFile temp_carried[256];

                        app.drop_count = 0;
                        s->drop_anim = 1.0f;

                        for (int i = 0; i < app.count; i++)
                        {
                                if ((drag_multi && s->selections[i]) || (!drag_multi && i == src))
                                {
                                        if (strcmp(app.entries[i].name, "..") != 0)
                                        {
                                                temp_carried[temp_carried_count].entry = app.entries[i];
                                                snprintf(temp_carried[temp_carried_count].path, PATH_MAX, "%s/%s", app.cwd, app.entries[i].name);
                                                if (app.drop_count < 64)
                                                        strcpy(app.drop_names[app.drop_count++], app.entries[i].name);
                                                temp_carried_count++;
                                        }
                                }
                        }

                        if (dst != -1 && app.entries[dst].is_dir && strcmp(app.entries[dst].name, ".") != 0)
                        {
                                UIRect r = ui_list_item_rect(s, dst);
                                s->drop_dst_x = r.x;
                                s->drop_dst_y = r.y;
                                s->drop_to_target = true;

                                for (int i = 0; i < temp_carried_count; i++)
                                {
                                        char new_path[PATH_MAX];
                                        snprintf(new_path, PATH_MAX, "%s/%s/%s", app.cwd, app.entries[dst].name, temp_carried[i].entry.name);
                                        rename(temp_carried[i].path, new_path);
                                }
                                strcpy(app.next_dir, ".");
                        }
                        else
                                s->drop_to_target = false;
                }

                ui_rect(0, 0, term_width, params.y, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);
                char header[PATH_MAX + 20];
                snprintf(header, sizeof(header), "%s ", app.cwd);
                ui_text(1, 0, header, (Color){0}, clr_bar, false, false);

                ui_rect(0, term_height - 1, term_width, 1, clr_bar);
                ui_text(1, term_height - 1, s->carrying ? " Arrows: Navigate | Enter: Drop into Folder | 'Tab': Drop Here | Space: Pick up/Drop | Esc: Cancel " : " Arrows: Navigate | Space: Select | Tab: Move | Esc: Cancel | 'q': Quit ", (Color){0}, clr_bar, false, false);

                const char *menu_options[] = {"Open", "Rename", "Delete", "Cancel"};
                int selected_action = -1;
                if (ui_context_do(menu_options, 4, &selected_action) && selected_action == 0)
                        if (app.entries[ui_context_target()].is_dir)
                                strcpy(app.next_dir, app.entries[ui_context_target()].name);

                int current_drag = s->is_dragging ? s->drag_idx : s->kb_drag_idx;

                if (s->fly_anim > 0.0f)
                {
                        float ease = s->fly_anim * s->fly_anim;
                        int fly_x = s->fly_is_pickup ? s->carry_x + (s->fly_origin_x - s->carry_x) * ease : s->fly_origin_x + (s->carry_x - s->fly_origin_x) * ease;
                        int fly_y = s->fly_is_pickup ? s->carry_y + (s->fly_origin_y - s->carry_y) * ease : s->fly_origin_y + (s->carry_y - s->fly_origin_y) * ease;

                        UIRect f_r = ui_list_item_rect(s, 0);
                        draw_item(&app, &app.fly_entry, fly_x, fly_y, f_r.w, f_r.h, false, false, false, false, false, false);
                }

                if (s->carrying || (current_drag >= 0 && current_drag < app.count && strcmp(app.entries[current_drag].name, "..")))
                {
                        int drag_count = s->carrying ? app.carried_count : 0;
                        if (!s->carrying && current_drag >= 0 && s->selections[current_drag])
                                for (int i = 0; i < app.count; i++)
                                        if (s->selections[i])
                                                drag_count++;

                        if (drag_count == 0 && !s->carrying)
                                drag_count = 1;

                        FileEntry *ghost_entry = s->carrying ? &app.carried[0].entry : &app.entries[current_drag];
                        draw_item(&app, ghost_entry, (int)s->carry_x, (int)s->carry_y, 14, 7, false, false, true, false, false, false);

                        if (drag_count > 1)
                        {
                                char badge[16];
                                snprintf(badge, sizeof(badge), " %d ", drag_count);
                                ui_text((int)s->carry_x + 10, (int)s->carry_y - 1, badge, clr_text, (Color){200, 50, 50}, true, false);
                        }
                }

                ui_end();

                if (s->is_dragging && !was_dragging)
                        s->pickup_anim = 1.0f;
                was_carrying = s->carrying;
                was_dragging = s->is_dragging;
                if (first_frame)
                        first_frame = false;
        }
        return 0;
}