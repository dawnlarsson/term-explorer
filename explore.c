#include "terminal.c"
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <ctype.h>

Color clr_bg, clr_bar = {170, 170, 170}, clr_text = {255, 255, 255}, clr_folder = {255, 255, 85}, clr_hover = {170, 170, 170}, clr_sel_bg = {40, 70, 120};

typedef struct
{
        char name[256];
        bool is_dir, is_exec;
        off_t size;
} FileEntry;

typedef struct
{
        FileEntry *entries;
        int capacity, count;
        char cwd[PATH_MAX], next_dir[PATH_MAX];
        UIListState list;
        bool quit;
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

        app->count = 0;
        ui_list_reset(&app->list);

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
                strcpy(e->name, dir->d_name); // Buffer size is structurally guaranteed
                e->size = st.st_size;
                e->is_dir = S_ISDIR(st.st_mode);
                e->is_exec = (st.st_mode & S_IXUSR) && !e->is_dir;
        }
        qsort(app->entries, app->count, sizeof(FileEntry), cmp_entries);
        if (app->count > 0)
                app->list.selected_idx = 0;
}

void draw_item_grid(AppState *app, int i, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target, bool is_multi_sel)
{
        Color item_bg = is_drop_target ? (Color){50, 150, 50} : (is_ghost ? (Color){30, 30, 30} : (is_multi_sel ? clr_sel_bg : (is_hover || is_sel ? clr_hover : clr_bg)));
        Color icon_fg = is_ghost ? (Color){100, 100, 100} : (app->entries[i].is_dir ? clr_folder : (app->entries[i].is_exec ? (Color){85, 255, 85} : clr_text));

        if (is_hover || is_sel || is_ghost || is_drop_target || is_multi_sel)
                ui_rect(x + 1, y, w - 2, h, item_bg);

        char ext[5] = ".   ";
        if (!app->entries[i].is_dir)
        {
                char *dot = strrchr(app->entries[i].name, '.');
                if (dot && dot > app->entries[i].name && strlen(dot + 1) < 4)
                {
                        for (int j = 0; dot[1 + j]; j++)
                                ext[1 + j] = toupper(dot[1 + j]);
                }
        }

        const char *icon_dir[] = {" ┌─┐____ ", " │ └────│ ", " │      │ ", " └──────┘ "};
        const char *icon_file[] = {"  ┌──┐_ ", "  │  └─│", "  │    │", "  └────┘"};
        const char *icon_exec[] = {"  ┌──┐_ ", "  │░░└─│", "  │░░░░│", "  └────┘"};
        const char **icon = app->entries[i].is_dir ? icon_dir : (app->entries[i].is_exec ? icon_exec : icon_file);

        int y_off = (is_hover && term_mouse.left && !app->list.is_dragging) ? 1 : 0;
        for (int j = 0; j < 4; j++)
                ui_text(x + 2, y + j + y_off, icon[j], icon_fg, item_bg, false, false);

        if (ext[1] != ' ' && !app->entries[i].is_dir)
                ui_text(x + 6, y + 2 + y_off, ext, icon_fg, item_bg, false, true);

        char l1[16], l2[16]; // Prism zero-initializes this, no need for manual '\0' padding!
        int mw = w - 2 > 15 ? 15 : (w - 2 < 0 ? 0 : w - 2);
        int len = strlen(app->entries[i].name);

        strncpy(l1, app->entries[i].name, mw);
        if (len > mw)
        {
                strncpy(l2, app->entries[i].name + mw, mw);
                if (len > mw * 2)
                {
                        l2[mw - 2] = '.';
                        l2[mw - 1] = '.';
                }
        }
        ui_text_centered(x + 1, y + 4 + y_off, mw, l1, is_ghost ? icon_fg : clr_text, item_bg, false, false);
        if (l2[0])
                ui_text_centered(x + 1, y + 5 + y_off, mw, l2, is_ghost ? icon_fg : clr_text, item_bg, false, false);
}

void draw_item_list(AppState *app, int i, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target, bool is_multi_sel)
{
        Color item_bg = is_drop_target ? (Color){50, 150, 50} : (is_ghost ? (Color){30, 30, 30} : (is_multi_sel ? clr_sel_bg : (is_hover || is_sel ? clr_hover : clr_bg)));
        Color icon_fg = is_ghost ? (Color){100, 100, 100} : (app->entries[i].is_dir ? clr_folder : (app->entries[i].is_exec ? (Color){85, 255, 85} : clr_text));

        ui_rect(x, y, w, 1, item_bg);
        ui_text(x + 1, y, app->entries[i].is_dir ? " ▓]" : " ■", icon_fg, item_bg, false, false);

        char l[256]; // Zero-initialized by Prism
        int copy_len = w - 9 > 255 ? 255 : (w - 9 < 0 ? 0 : w - 9);
        strncpy(l, app->entries[i].name, copy_len);
        ui_text(x + 8, y, l, is_ghost ? icon_fg : clr_text, item_bg, false, false);

        char size_str[32]; // Zero-initialized by Prism
        if (!app->entries[i].is_dir)
        {
                off_t s = app->entries[i].size;
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

void handle_input(AppState *app, int key, const UIListParams *params)
{
        if (key == 'q')
                app->quit = true;
        if (key == KEY_ESC && !app->list.is_kb_dragging)
        {
                bool has_selections = false;
                for (int i = 0; i < app->count; i++)
                        if (app->list.selections[i])
                                has_selections = true;
                if (!has_selections)
                        app->quit = true;
        }
        if (key == 'v')
                ui_list_set_mode(&app->list, params, !app->list.mode);
        if (key == KEY_BACKSPACE && !app->list.is_kb_dragging)
                strcpy(app->next_dir, "..");
        if (key == KEY_ENTER && !app->list.is_kb_dragging && app->count > 0 && app->list.selected_idx >= 0 && app->entries[app->list.selected_idx].is_dir)
                strcpy(app->next_dir, app->entries[app->list.selected_idx].name);
}

int main(void)
{
        term_init() orelse return 1;
        defer term_restore();

        AppState app;
        defer free(app.entries);
        app_load_dir(&app, ".");

        bool first_frame = true;

        while (!app.quit)
        {
                int timeout = (app.next_dir[0] || ui_list_is_animating(&app.list) || app.list.is_dragging || app.list.is_kb_dragging || app.list.is_box_selecting) ? term_anim_timeout : 1000;
                if (first_frame)
                {
                        timeout = 0;
                        first_frame = false;
                }

                int key = term_poll(timeout);
                UIListParams params = {0, 1, term_width, term_height - 2 > 0 ? term_height - 2 : 1, app.count, 14, 7, clr_bg, {30, 30, 30}, clr_bar};
                handle_input(&app, key, &params);

                ui_begin();
                ui_clear(clr_bg);
                ui_list_begin(&app.list, &params, key);

                for (int i = 0; i < app.count; i++)
                {
                        UIItemResult item; // Zero-initialized by Prism!
                        ui_list_do_item(&app.list, i, &item) orelse continue;

                        if (!strcmp(app.entries[i].name, ".."))
                        {
                                app.list.selections[i] = false;
                                item.is_selected = false;
                                item.is_ghost = false;
                        }

                        int current_drag = app.list.is_dragging ? app.list.drag_idx : app.list.kb_drag_idx;
                        bool is_sel = (i == app.list.selected_idx);
                        bool valid_drop = item.is_drop_target && app.entries[i].is_dir;
                        bool is_ghost = item.is_ghost || (current_drag != -1 && app.list.selections[current_drag] && item.is_selected);

                        if (app.list.mode == UI_MODE_GRID)
                                draw_item_grid(&app, i, item.x, item.y, item.w, item.h, is_sel, item.hovered, is_ghost, valid_drop, item.is_selected);
                        else
                                draw_item_list(&app, i, item.x, item.y, item.w, item.h, is_sel, item.hovered, is_ghost, valid_drop, item.is_selected);

                        if (item.right_clicked)
                                ui_context_open(i);
                }

                ui_list_end(&app.list);

                if (app.list.action_click_idx != -1 && app.entries[app.list.action_click_idx].is_dir)
                        strcpy(app.next_dir, app.entries[app.list.action_click_idx].name);

                if (app.list.action_drop_src != -1 && app.list.action_drop_dst != -1)
                {
                        int src = app.list.action_drop_src, dst = app.list.action_drop_dst;
                        if (app.entries[dst].is_dir && strcmp(app.entries[src].name, ".."))
                        {
                                bool drag_multi = app.list.selections[src];
                                for (int i = 0; i < app.count; i++)
                                {
                                        if ((drag_multi && app.list.selections[i]) || (!drag_multi && i == src))
                                        {
                                                if (i == dst || !strcmp(app.entries[i].name, ".."))
                                                        continue;
                                                char old_path[PATH_MAX], new_path[PATH_MAX]; // Zero-initialized automatically
                                                snprintf(old_path, sizeof(old_path), "%s/%s", app.cwd, app.entries[i].name);
                                                snprintf(new_path, sizeof(new_path), "%s/%s/%s", app.cwd, app.entries[dst].name, app.entries[i].name);
                                                rename(old_path, new_path);
                                        }
                                }
                                strcpy(app.next_dir, ".");
                        }
                }

                ui_rect(0, 0, term_width, params.y, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);
                char header[PATH_MAX + 20];
                snprintf(header, sizeof(header), "%s ", app.cwd);
                ui_text(1, 0, header, (Color){0}, clr_bar, false, false);

                ui_rect(0, term_height - 1, term_width, 1, clr_bar);
                ui_text(1, term_height - 1, " Arrows: Navigate | Space: Select | Tab: Move | Esc: Cancel | 'q': Quit ", (Color){0}, clr_bar, false, false);

                const char *menu_options[] = {"Open", "Rename", "Delete", "Cancel"};
                int selected_action = -1;
                if (ui_context_do(menu_options, 4, &selected_action) && selected_action == 0)
                {
                        if (app.entries[ui_context_target()].is_dir)
                                strcpy(app.next_dir, app.entries[ui_context_target()].name);
                }

                int current_drag = app.list.is_dragging ? app.list.drag_idx : app.list.kb_drag_idx;
                if (current_drag >= 0 && current_drag < app.count && strcmp(app.entries[current_drag].name, ".."))
                {
                        int drag_count = 0;
                        if (app.list.selections[current_drag])
                                for (int i = 0; i < app.count; i++)
                                        if (app.list.selections[i])
                                                drag_count++;

                        int fx = app.list.is_dragging ? term_mouse.x - app.list.drag_off_x : (int)app.list.kb_drag_x;
                        int fy = app.list.is_dragging ? term_mouse.y - app.list.drag_off_y : (int)app.list.kb_drag_y;
                        int drag_w = params.cell_w;

                        if (app.list.mode == UI_MODE_GRID)
                        {
                                draw_item_grid(&app, current_drag, fx, fy, drag_w, params.cell_h, false, false, false, false, false);
                        }
                        else
                        {
                                drag_w = strlen(app.entries[current_drag].name) + 14;
                                if (drag_w < 20)
                                        drag_w = 20;
                                if (app.list.is_dragging)
                                        fx = term_mouse.x - (app.list.drag_off_x > drag_w - 3 ? drag_w - 3 : app.list.drag_off_x);
                                draw_item_list(&app, current_drag, fx, fy, drag_w, 1, false, false, false, false, false);
                        }

                        if (drag_count > 1)
                        {
                                char badge[16];
                                snprintf(badge, sizeof(badge), " +%d ", drag_count - 1);
                                ui_text(app.list.mode == UI_MODE_GRID ? fx + drag_w - 4 : fx + drag_w + 1, app.list.mode == UI_MODE_GRID ? fy - 1 : fy, badge, clr_text, (Color){200, 50, 50}, true, false);
                        }
                }

                ui_end();

                if (app.next_dir[0])
                {
                        app_load_dir(&app, app.next_dir);
                        app.next_dir[0] = '\0';
                        first_frame = true;
                }
        }
        return 0;
}