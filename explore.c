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
        bool carrying;

        char drop_names[64][256];
        int drop_count;
        float drop_anim;
        float carry_x, carry_y;

        int drop_dst_x, drop_dst_y;
        bool is_folder_drop;

        int last_hovered_idx;
        FileEntry fly_entry;
        float fly_anim;
        bool fly_is_pickup;
        int fly_origin_x;
        int fly_origin_y;
        float pickup_anim;
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

        char selected_name[256];
        int old_idx = app->list.selected_idx;

        if (app->count > 0 && old_idx >= 0 && old_idx < app->count)
                strcpy(selected_name, app->entries[old_idx].name);

        app->count = 0;
        UIListMode old_mode = app->list.mode;
        ui_list_reset(&app->list);
        app->list.mode = old_mode;

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

        if (app->count > 0)
        {
                app->list.selected_idx = old_idx >= app->count ? app->count - 1 : (old_idx < 0 ? 0 : old_idx);
                if (selected_name[0])
                {
                        for (int i = 0; i < app->count; i++)
                        {
                                if (!strcmp(app->entries[i].name, selected_name))
                                {
                                        app->list.selected_idx = i;
                                        break;
                                }
                        }
                }
        }
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
                {
                        for (int j = 0; dot[1 + j]; j++)
                                ext[1 + j] = toupper(dot[1 + j]);
                }
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

void handle_input(AppState *app, int *key, const UIListParams *params)
{
        if (*key == 'q' && !app->carrying)
                app->quit = true;
        if (*key == KEY_ESC)
        {
                app->carrying = false;
                ui_list_clear_selections(&app->list);
        }
        if (*key == 'v')
                ui_list_set_mode(&app->list, params, !app->list.mode);
        if (*key == KEY_BACKSPACE && !app->carrying)
                strcpy(app->next_dir, "..");

        if (*key == KEY_ENTER && app->count > 0 && app->list.selected_idx >= 0 && app->entries[app->list.selected_idx].is_dir)
        {
                strcpy(app->next_dir, app->entries[app->list.selected_idx].name);
                *key = 0;
        }

        if (*key == ' ' && app->carrying)
        {
                int src = app->list.selected_idx != -1 ? app->list.selected_idx : app->last_hovered_idx;
                if (src >= 0 && src < app->count && strcmp(app->entries[src].name, "..") != 0)
                {
                        int found_idx = -1;
                        for (int i = 0; i < app->carried_count; i++)
                        {
                                if (!strcmp(app->carried[i].entry.name, app->entries[src].name))
                                {
                                        found_idx = i;
                                        break;
                                }
                        }

                        int cols = (app->list.mode == UI_MODE_LIST) ? 1 : ((params->w - 1) / params->cell_w > 0 ? (params->w - 1) / params->cell_w : 1);
                        int c_w = (app->list.mode == UI_MODE_LIST) ? params->w - 1 : params->cell_w;
                        int c_h = (app->list.mode == UI_MODE_LIST) ? 1 : params->cell_h;
                        int scroll_int = (int)(app->list.current_scroll + 0.5f);
                        app->fly_origin_x = params->x + (src % cols) * c_w;
                        app->fly_origin_y = params->y + (src / cols * c_h) - scroll_int;
                        app->fly_entry = app->entries[src];
                        app->fly_anim = 1.0f;

                        if (found_idx != -1)
                        {
                                app->fly_is_pickup = false;
                                for (int i = found_idx; i < app->carried_count - 1; i++)
                                        app->carried[i] = app->carried[i + 1];
                                app->carried_count--;
                                if (app->carried_count == 0)
                                        app->carrying = false;
                        }
                        else if (app->carried_count < 256)
                        {
                                app->fly_is_pickup = true;
                                app->carried[app->carried_count].entry = app->entries[src];
                                snprintf(app->carried[app->carried_count].path, PATH_MAX, "%s/%s", app->cwd, app->entries[src].name);
                                app->carried_count++;
                        }
                }
                *key = 0;
        }

        if (*key == '\t')
        {
                if (app->carrying)
                {
                        app->drop_count = 0;
                        app->drop_anim = 1.0f;
                        app->is_folder_drop = false;
                        for (int i = 0; i < app->carried_count; i++)
                        {
                                char new_path[PATH_MAX];
                                snprintf(new_path, PATH_MAX, "%s/%s", app->cwd, app->carried[i].entry.name);
                                rename(app->carried[i].path, new_path);
                                if (app->drop_count < 64)
                                        strcpy(app->drop_names[app->drop_count++], app->carried[i].entry.name);
                        }
                        app->carrying = false;
                        strcpy(app->next_dir, ".");
                }
                else
                {
                        app->carried_count = 0;
                        bool drag_multi = false;
                        for (int i = 0; i < app->count; i++)
                                if (app->list.selections[i])
                                        drag_multi = true;

                        int src = app->list.selected_idx != -1 ? app->list.selected_idx : app->last_hovered_idx;
                        if (src == -1)
                                src = 0;

                        for (int i = 0; i < app->count; i++)
                        {
                                if ((drag_multi && app->list.selections[i]) || (!drag_multi && i == src))
                                {
                                        if (strcmp(app->entries[i].name, "..") != 0)
                                        {
                                                app->carried[app->carried_count].entry = app->entries[i];
                                                snprintf(app->carried[app->carried_count].path, PATH_MAX, "%s/%s", app->cwd, app->entries[i].name);
                                                app->carried_count++;
                                        }
                                }
                        }
                        if (app->carried_count > 0)
                        {
                                app->carrying = true;
                                app->pickup_anim = 1.0f;
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

        bool first_frame = true;
        bool was_carrying;
        bool was_dragging;

        while (!app.quit)
        {
                int timeout = (app.next_dir[0] || ui_list_is_animating(&app.list) || app.list.is_dragging || app.list.is_kb_dragging || app.list.is_box_selecting || app.carrying || app.drop_anim > 0.0f || app.fly_anim > 0.0f || app.pickup_anim > 0.0f) ? term_anim_timeout : 1000;
                if (first_frame)
                        timeout = 0;

                int key = term_poll(timeout);
                UIListParams params = {0, 1, term_width, term_height - 2 > 0 ? term_height - 2 : 1, app.count, 14, 7, clr_bg, {30, 30, 30}, clr_bar};
                handle_input(&app, &key, &params);

                if (app.next_dir[0] && app.drop_anim <= 0.01f)
                {
                        app_load_dir(&app, app.next_dir);
                        app.next_dir[0] = '\0';
                        first_frame = true;
                        continue;
                }

                ui_begin();
                ui_clear(clr_bg);
                ui_list_begin(&app.list, &params, key);

                int active_idx = app.list.selected_idx != -1 ? app.list.selected_idx : app.last_hovered_idx;
                if (active_idx == -1)
                        active_idx = 0;

                int cols = (app.list.mode == UI_MODE_LIST) ? 1 : ((params.w - 1) / params.cell_w > 0 ? (params.w - 1) / params.cell_w : 1);
                int c_w = (app.list.mode == UI_MODE_LIST) ? params.w - 1 : params.cell_w;
                int c_h = (app.list.mode == UI_MODE_LIST) ? 1 : params.cell_h;
                int scroll_int = (int)(app.list.current_scroll + 0.5f);

                int sel_screen_x = params.x + (active_idx % cols) * c_w;
                int sel_screen_y = params.y + (active_idx / cols * c_h) - scroll_int;

                if (app.carrying)
                {
                        float target_x = sel_screen_x + (app.list.mode == UI_MODE_LIST ? 45 : params.cell_w / 2);
                        float target_y = sel_screen_y + (app.list.mode == UI_MODE_LIST ? -1 : params.cell_h / 2);

                        if (!was_carrying)
                        {
                                app.carry_x = sel_screen_x;
                                app.carry_y = sel_screen_y;
                        }

                        app.carry_x += (target_x - app.carry_x) * anim_speed_carry;
                        app.carry_y += (target_y - app.carry_y) * anim_speed_carry;
                }
                else if (app.list.is_dragging)
                {
                        app.carry_x = term_mouse.x - app.list.drag_off_x;
                        app.carry_y = term_mouse.y - app.list.drag_off_y;
                        if (app.list.mode == UI_MODE_LIST)
                        {
                                app.carry_x = term_mouse.x - 2;
                                app.carry_y = term_mouse.y - 1;
                        }
                }
                else if (app.list.is_kb_dragging)
                {
                        app.carry_x = app.list.kb_drag_x;
                        app.carry_y = app.list.kb_drag_y;
                }

                // --- BASE LAYER PASS ---
                for (int i = 0; i < app.count; i++)
                {
                        UIItemResult item;
                        ui_list_do_item(&app.list, i, &item) orelse continue;

                        if (item.hovered)
                                app.last_hovered_idx = i;

                        if (!strcmp(app.entries[i].name, ".."))
                        {
                                app.list.selections[i] = false;
                                item.is_selected = false;
                                item.is_ghost = false;
                        }

                        int current_drag = app.list.is_dragging ? app.list.drag_idx : app.list.kb_drag_idx;
                        bool is_sel = (i == app.list.selected_idx);
                        bool valid_drop = item.is_drop_target && app.entries[i].is_dir;

                        bool is_carried = false;
                        if (app.carrying)
                        {
                                for (int c = 0; c < app.carried_count; c++)
                                {
                                        if (!strcmp(app.entries[i].name, app.carried[c].entry.name))
                                        {
                                                is_carried = true;
                                                break;
                                        }
                                }
                        }

                        bool is_picked_up_mouse = (!app.carrying && current_drag != -1 && (app.list.selections[current_drag] ? item.is_selected : current_drag == i));
                        bool is_ghost = item.is_ghost || is_picked_up_mouse || is_carried;

                        bool is_dropped = false;
                        if (app.drop_anim > 0.01f)
                        {
                                for (int d = 0; d < app.drop_count; d++)
                                {
                                        if (!strcmp(app.entries[i].name, app.drop_names[d]))
                                        {
                                                is_dropped = true;
                                                break;
                                        }
                                }
                        }

                        if (!is_dropped) // Skip drawing dropped items in the base layer
                        {
                                if (app.list.mode == UI_MODE_GRID)
                                        draw_item_grid(&app, &app.entries[i], item.x, item.y, item.w, item.h, is_sel, item.hovered, is_ghost, valid_drop, item.is_selected, false);
                                else
                                        draw_item_list(&app, &app.entries[i], item.x, item.y, item.w, item.h, is_sel, item.hovered, is_ghost, valid_drop, item.is_selected, false);
                        }

                        if (item.right_clicked)
                                ui_context_open(i);
                }

                ui_list_end(&app.list);
                
                for (int i = 0; i < app.count; i++)
                {
                        int current_drag = app.list.is_dragging ? app.list.drag_idx : app.list.kb_drag_idx;

                        bool is_carried = false;
                        if (app.carrying)
                        {
                                for (int c = 0; c < app.carried_count; c++)
                                {
                                        if (!strcmp(app.entries[i].name, app.carried[c].entry.name))
                                        {
                                                is_carried = true;
                                                break;
                                        }
                                }
                        }

                        bool is_picked_up_mouse = (!app.carrying && current_drag != -1 && (app.list.selections[current_drag] ? app.list.selections[i] : current_drag == i));

                        bool is_dropped = false;
                        if (app.drop_anim > 0.01f)
                        {
                                for (int d = 0; d < app.drop_count; d++)
                                {
                                        if (!strcmp(app.entries[i].name, app.drop_names[d]))
                                        {
                                                is_dropped = true;
                                                break;
                                        }
                                }
                        }

                        if (is_dropped || ((is_carried || is_picked_up_mouse) && app.pickup_anim > 0.01f))
                        {
                                int base_x = params.x + (i % cols) * c_w;
                                int base_y = params.y + (i / cols * c_h) - scroll_int;

                                if (is_dropped)
                                {
                                        float ease = app.drop_anim * app.drop_anim;
                                        int r_x = base_x, r_y = base_y;
                                        if (app.is_folder_drop)
                                        {
                                                r_x = app.drop_dst_x + (int)((app.carry_x - app.drop_dst_x) * ease);
                                                r_y = app.drop_dst_y + (int)((app.carry_y - app.drop_dst_y) * ease);
                                        }
                                        else
                                        {
                                                r_x += (int)((app.carry_x - base_x) * ease);
                                                r_y += (int)((app.carry_y - base_y) * ease);
                                        }
                                        if (app.list.mode == UI_MODE_GRID)
                                                draw_item_grid(&app, &app.entries[i], r_x, r_y, c_w, c_h, false, false, false, false, false, true);
                                        else
                                                draw_item_list(&app, &app.entries[i], r_x, r_y, c_w, c_h, false, false, false, false, false, true);
                                }

                                if ((is_carried || is_picked_up_mouse) && app.pickup_anim > 0.01f)
                                {
                                        float ease = app.pickup_anim * app.pickup_anim;
                                        int fly_x = app.carry_x + (base_x - app.carry_x) * ease;
                                        int fly_y = app.carry_y + (base_y - app.carry_y) * ease;
                                        if (app.list.mode == UI_MODE_GRID)
                                                draw_item_grid(&app, &app.entries[i], fly_x, fly_y, c_w, c_h, false, false, false, false, false, false);
                                        else
                                                draw_item_list(&app, &app.entries[i], fly_x, fly_y, c_w, c_h, false, false, false, false, false, false);
                                }
                        }
                }

                if (app.list.action_click_idx != -1 && app.entries[app.list.action_click_idx].is_dir)
                        strcpy(app.next_dir, app.entries[app.list.action_click_idx].name);

                if (app.list.action_drop_src != -1)
                {
                        int src = app.list.action_drop_src, dst = app.list.action_drop_dst;

                        bool drag_multi;
                        if (src >= 0 && app.list.selections[src])
                        {
                                for (int i = 0; i < app.count; i++)
                                        if (app.list.selections[i])
                                                drag_multi = true;
                        }

                        int temp_carried_count;
                        raw CarriedFile temp_carried[256];

                        app.drop_count = 0;
                        app.drop_anim = 1.0f;

                        for (int i = 0; i < app.count; i++)
                        {
                                if ((drag_multi && app.list.selections[i]) || (!drag_multi && i == src))
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
                                app.drop_dst_x = params.x + (dst % cols) * c_w;
                                app.drop_dst_y = params.y + (dst / cols * c_h) - scroll_int;
                                app.is_folder_drop = true;

                                for (int i = 0; i < temp_carried_count; i++)
                                {
                                        char new_path[PATH_MAX];
                                        snprintf(new_path, PATH_MAX, "%s/%s/%s", app.cwd, app.entries[dst].name, temp_carried[i].entry.name);
                                        rename(temp_carried[i].path, new_path);
                                }
                                strcpy(app.next_dir, ".");
                        }
                        else
                        {
                                app.is_folder_drop = false;
                        }
                }

                ui_rect(0, 0, term_width, params.y, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);
                char header[PATH_MAX + 20];
                snprintf(header, sizeof(header), "%s ", app.cwd);
                ui_text(1, 0, header, (Color){0}, clr_bar, false, false);

                ui_rect(0, term_height - 1, term_width, 1, clr_bar);
                ui_text(1, term_height - 1, app.carrying ? " Arrows: Navigate | Enter: Open Folder | 'Tab': Drop Files | Space: Pick up/Drop | Esc: Cancel " : " Arrows: Navigate | Space: Select | Tab: Move | Esc: Cancel | 'q': Quit ", (Color){0}, clr_bar, false, false);

                const char *menu_options[] = {"Open", "Rename", "Delete", "Cancel"};
                int selected_action = -1;
                if (ui_context_do(menu_options, 4, &selected_action) && selected_action == 0)
                {
                        if (app.entries[ui_context_target()].is_dir)
                                strcpy(app.next_dir, app.entries[ui_context_target()].name);
                }

                int current_drag = app.list.is_dragging ? app.list.drag_idx : app.list.kb_drag_idx;

                if (app.fly_anim > 0.0f)
                {
                        float ease = app.fly_anim * app.fly_anim;
                        int fly_x, fly_y;

                        if (app.fly_is_pickup)
                        {
                                fly_x = app.carry_x + (app.fly_origin_x - app.carry_x) * ease;
                                fly_y = app.carry_y + (app.fly_origin_y - app.carry_y) * ease;
                        }
                        else
                        {
                                fly_x = app.fly_origin_x + (app.carry_x - app.fly_origin_x) * ease;
                                fly_y = app.fly_origin_y + (app.carry_y - app.fly_origin_y) * ease;
                        }

                        int f_w = (app.list.mode == UI_MODE_LIST) ? params.w - 1 : params.cell_w;
                        int f_h = (app.list.mode == UI_MODE_LIST) ? 1 : params.cell_h;

                        if (app.list.mode == UI_MODE_GRID)
                                draw_item_grid(&app, &app.fly_entry, fly_x, fly_y, f_w, f_h, false, false, false, false, false, false);
                        else
                                draw_item_list(&app, &app.fly_entry, fly_x, fly_y, f_w, f_h, false, false, false, false, false, false);

                        app.fly_anim -= anim_speed_fly * term_dt_scale;
                        if (app.fly_anim < 0.0f)
                                app.fly_anim = 0.0f;
                }

                if (app.carrying || (current_drag >= 0 && current_drag < app.count && strcmp(app.entries[current_drag].name, "..")))
                {
                        int drag_count = app.carrying ? app.carried_count : 0;
                        if (!app.carrying && current_drag >= 0 && app.list.selections[current_drag])
                                for (int i = 0; i < app.count; i++)
                                        if (app.list.selections[i])
                                                drag_count++;

                        if (drag_count == 0 && !app.carrying)
                                drag_count = 1;

                        int drag_w = 14;
                        int drag_h = 7;

                        int fx = (int)app.carry_x;
                        int fy = (int)app.carry_y;

                        FileEntry *ghost_entry = app.carrying ? &app.carried[0].entry : &app.entries[current_drag];

                        draw_item_grid(&app, ghost_entry, fx, fy, drag_w, drag_h, false, false, true, false, false, false);

                        if (drag_count > 1)
                        {
                                char badge[16];
                                snprintf(badge, sizeof(badge), " %d ", drag_count);
                                ui_text(fx + drag_w - 4, fy - 1, badge, clr_text, (Color){200, 50, 50}, true, false);
                        }
                }

                ui_end();

                if (app.list.is_dragging && !was_dragging)
                        app.pickup_anim = 1.0f;

                if (app.pickup_anim > 0.0f)
                {
                        app.pickup_anim -= anim_speed_fly * term_dt_scale;
                        if (app.pickup_anim < 0.0f)
                                app.pickup_anim = 0.0f;
                }

                if (app.drop_anim > 0.0f)
                {
                        app.drop_anim -= anim_speed_drop * term_dt_scale;
                        if (app.drop_anim < 0.0f)
                                app.drop_anim = 0.0f;
                }

                was_carrying = app.carrying;
                was_dragging = app.list.is_dragging;

                if (first_frame)
                        first_frame = false;
        }
        return 0;
}