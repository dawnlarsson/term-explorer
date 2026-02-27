#include "terminal.c"
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

Color clr_bg = {0, 0, 0}, clr_bar = {170, 170, 170}, clr_text = {255, 255, 255}, clr_folder = {255, 255, 85}, clr_hover = {170, 170, 170};

typedef struct
{
        char name[256];
        bool is_dir;
        bool is_exec;
        off_t size;
} FileEntry;

typedef struct
{
        FileEntry *entries;
        int capacity;
        int count;
        char cwd[PATH_MAX];
        char next_dir[PATH_MAX];
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

        (chdir(path) != -1) orelse return;
        getcwd(app->cwd, sizeof(app->cwd)) orelse return;

        app->count = 0;
        ui_list_reset(&app->list);

        for (;;)
        {
                struct dirent *dir = readdir(d) orelse break;
                if (!strcmp(dir->d_name, "."))
                        continue;

                if (app->count >= app->capacity)
                {
                        int new_cap = app->capacity ? app->capacity * 2 : 256;
                        void *tmp = realloc(app->entries, new_cap * sizeof(FileEntry)) orelse break;
                        app->capacity = new_cap;
                        app->entries = tmp;
                }

                raw struct stat st;
                (stat(dir->d_name, &st) == 0) orelse continue;

                strcpy(app->entries[app->count].name, dir->d_name);
                app->entries[app->count].size = st.st_size;
                app->entries[app->count].is_dir = S_ISDIR(st.st_mode);
                app->entries[app->count++].is_exec = (st.st_mode & S_IXUSR) && !S_ISDIR(st.st_mode);
        }
        qsort(app->entries, app->count, sizeof(FileEntry), cmp_entries);

        if (app->count > 0)
                app->list.selected_idx = 0;
}

void draw_item_grid(AppState *app, int i, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target)
{
        Color item_bg = is_drop_target ? (Color){50, 150, 50} : (is_ghost ? (Color){30, 30, 30} : (is_hover || is_sel ? clr_hover : clr_bg));
        Color icon_fg = app->entries[i].is_dir ? clr_folder : (app->entries[i].is_exec ? (Color){85, 255, 85} : clr_text);

        if (is_ghost)
                icon_fg = (Color){100, 100, 100};

        if (is_hover || is_sel || is_ghost || is_drop_target)
                ui_rect(x + 1, y, w - 2, h, item_bg);

        char disp[256];
        strncpy(disp, app->entries[i].name, 255);
        disp[255] = '\0';

        char ext[5] = ".   ";
        int elen;

        if (!app->entries[i].is_dir)
        {
                char *dot = strrchr(disp, '.');
                if (dot && dot > disp && strlen(dot + 1) < 4)
                {
                        elen = strlen(dot + 1);
                        for (int j = 0; j < elen; j++)
                                ext[1 + j] = (dot[1 + j] >= 'a' && dot[1 + j] <= 'z') ? dot[1 + j] - 32 : dot[1 + j];
                        *dot = '\0';
                }
        }

        const char *icon_dir[] = {" ┌─┐____ ", " │ └────│ ", " │      │ ", " └──────┘ "};
        const char *icon_file[] = {"  ┌──┐_ ", "  │  └─│", "  │    │", "  └────┘"};
        const char *icon_exec[] = {"  ┌──┐_ ", "  │░░└─│", "  │░░░░│", "  └────┘"};
        const char **icon = app->entries[i].is_dir ? icon_dir : (app->entries[i].is_exec ? icon_exec : icon_file);

        int y_off = (is_hover && term_mouse.left && !app->list.is_dragging) ? 1 : 0;

        for (int j = 0; j < 4; j++)
                ui_text(x + 2, y + j + y_off, icon[j], icon_fg, item_bg, false, false);

        if (elen > 0 && !app->entries[i].is_dir)
                ui_text(x + 6, y + 2 + y_off, ext, icon_fg, item_bg, false, true);

        char l1[16], l2[16];
        int mw = w - 2;
        if (mw > 15)
                mw = 15;
        if (mw < 0)
                mw = 0;
        int len = strlen(disp);

        strncpy(l1, disp, mw);
        l1[mw] = '\0';

        if (len > mw)
        {
                strncpy(l2, disp + mw, mw);
                l2[mw] = '\0';
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

void draw_item_list(AppState *app, int i, int x, int y, int w, int h, bool is_sel, bool is_hover, bool is_ghost, bool is_drop_target)
{
        Color item_bg = is_drop_target ? (Color){50, 150, 50} : (is_ghost ? (Color){30, 30, 30} : (is_hover || is_sel ? clr_hover : clr_bg));
        Color icon_fg = app->entries[i].is_dir ? clr_folder : (app->entries[i].is_exec ? (Color){85, 255, 85} : clr_text);
        if (is_ghost)
                icon_fg = (Color){100, 100, 100};

        ui_rect(x, y, w, 1, item_bg);
        ui_text(x + 1, y, app->entries[i].is_dir ? " ▓]" : (app->entries[i].is_exec ? " ■" : " ■"), icon_fg, item_bg, false, false);

        char l[256];
        int copy_len = w - 9 > 255 ? 255 : w - 9;
        if (copy_len < 0)
                copy_len = 0;
        strncpy(l, app->entries[i].name, copy_len);
        l[copy_len] = '\0';

        ui_text(x + 8, y, l, is_ghost ? icon_fg : clr_text, item_bg, false, false);

        char size_str[32];
        if (!app->entries[i].is_dir)
        {
                if (app->entries[i].size < 1024)
                        snprintf(size_str, 32, "%4lld B ", (long long)app->entries[i].size);
                else if (app->entries[i].size < 1024 * 1024)
                        snprintf(size_str, 32, "%4lld KB", (long long)(app->entries[i].size / 1024));
                else if (app->entries[i].size < 1024 * 1024 * 1024)
                        snprintf(size_str, 32, "%4lld MB", (long long)(app->entries[i].size / (1024 * 1024)));
                else
                        snprintf(size_str, 32, "%4lld GB", (long long)(app->entries[i].size / (1024 * 1024 * 1024)));
                ui_text(x + w - 10, y, size_str, is_ghost ? icon_fg : clr_bar, item_bg, false, false);
        }
}

void handle_input(AppState *app, int key, const UIListParams *params)
{
        if (key == 'q' || (key == KEY_ESC && !app->list.is_kb_dragging))
                app->quit = true;
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
                int timeout = (ui_list_is_animating(&app.list) || app.list.is_dragging || app.list.is_kb_dragging) ? term_anim_timeout : 1000;
                if (first_frame)
                {
                        timeout = 0;
                        first_frame = false;
                }

                int key = term_poll(timeout);

                UIListParams params = {
                    .x = 0, .y = 1, .w = term_width, .h = term_height - 2 > 0 ? term_height - 2 : 1, .item_count = app.count, .cell_w = 14, .cell_h = 7, .bg = clr_bg, .scrollbar_bg = (Color){30, 30, 30}, .scrollbar_fg = clr_bar};

                handle_input(&app, key, &params);

                ui_begin();
                ui_clear(clr_bg);

                ui_list_begin(&app.list, &params, key);

                for (int i = 0; i < app.count; i++)
                {
                        UIItemResult item;
                        ui_list_do_item(&app.list, i, &item) orelse continue;

                        bool is_sel = (i == app.list.selected_idx);
                        bool valid_drop = item.is_drop_target && app.entries[i].is_dir;
                        bool is_ghost = item.is_ghost && strcmp(app.entries[i].name, "..") != 0;

                        if (app.list.mode == UI_MODE_GRID)
                                draw_item_grid(&app, i, item.x, item.y, item.w, item.h, is_sel, item.hovered, is_ghost, valid_drop);
                        else
                                draw_item_list(&app, i, item.x, item.y, item.w, item.h, is_sel, item.hovered, is_ghost, valid_drop);

                        if (item.right_clicked)
                                ui_context_open(i);
                }

                ui_list_end(&app.list);

                if (app.list.action_click_idx != -1)
                {
                        if (app.entries[app.list.action_click_idx].is_dir)
                                strcpy(app.next_dir, app.entries[app.list.action_click_idx].name);
                }

                if (app.list.action_drop_src != -1 && app.list.action_drop_dst != -1)
                {
                        int src = app.list.action_drop_src;
                        int dst = app.list.action_drop_dst;

                        if (app.entries[dst].is_dir && strcmp(app.entries[src].name, "..") != 0)
                        {
                                char old_path[PATH_MAX], new_path[PATH_MAX];
                                snprintf(old_path, sizeof(old_path), "%s/%s", app.cwd, app.entries[src].name);
                                snprintf(new_path, sizeof(new_path), "%s/%s/%s", app.cwd, app.entries[dst].name, app.entries[src].name);

                                rename(old_path, new_path);
                                strcpy(app.next_dir, ".");
                        }
                }

                ui_rect(0, 0, term_width, params.y, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);
                char header[PATH_MAX + 20];
                snprintf(header, sizeof(header), "%s ", app.cwd);
                ui_text(1, 0, header, (Color){0, 0, 0}, clr_bar, false, false);

                ui_rect(0, term_height - 1, term_width, 1, clr_bar);
                ui_text(1, term_height - 1, " Arrows: Navigate | v: Toggle View | Backspace: Up | Tab: Move | 'q': Quit ", (Color){0, 0, 0}, clr_bar, false, false);

                const char *menu_options[] = {"Open", "Rename", "Delete", "Cancel"};
                int selected_action = -1;

                if (ui_context_do(menu_options, 4, &selected_action))
                {
                        FileEntry *target = &app.entries[ui_context_target()];
                        switch (selected_action)
                        {
                        case 0:
                                if (target->is_dir)
                                        strcpy(app.next_dir, target->name);
                                break;
                        case 1:
                                break;
                        case 2:
                                break;
                        }
                }

                if (app.list.is_dragging && app.list.drag_idx >= 0 && app.list.drag_idx < app.count && strcmp(app.entries[app.list.drag_idx].name, "..") != 0)
                {
                        int drag_idx = app.list.drag_idx;
                        if (app.list.mode == UI_MODE_GRID)
                        {
                                int fx = term_mouse.x - app.list.drag_off_x;
                                int fy = term_mouse.y - app.list.drag_off_y;
                                draw_item_grid(&app, drag_idx, fx, fy, params.cell_w, params.cell_h, false, false, false, false);
                        }
                        else
                        {
                                int drag_w = strlen(app.entries[drag_idx].name) + 14;
                                if (drag_w < 20)
                                        drag_w = 20;

                                int off_x = app.list.drag_off_x;
                                if (off_x > drag_w - 3)
                                        off_x = drag_w - 3;

                                int fx = term_mouse.x - off_x;
                                int fy = term_mouse.y - app.list.drag_off_y;

                                draw_item_list(&app, drag_idx, fx, fy, drag_w, 1, false, false, false, false);
                        }
                }
                else if (app.list.is_kb_dragging && app.list.kb_drag_idx >= 0 && app.list.kb_drag_idx < app.count && strcmp(app.entries[app.list.kb_drag_idx].name, "..") != 0)
                {
                        int drag_idx = app.list.kb_drag_idx;
                        int fx = (int)app.list.kb_drag_x;
                        int fy = (int)app.list.kb_drag_y;

                        if (app.list.mode == UI_MODE_GRID)
                        {
                                draw_item_grid(&app, drag_idx, fx, fy, params.cell_w, params.cell_h, false, false, false, false);
                        }
                        else
                        {
                                int drag_w = strlen(app.entries[drag_idx].name) + 14;
                                if (drag_w < 20)
                                        drag_w = 20;

                                draw_item_list(&app, drag_idx, fx, fy, drag_w, 1, false, false, false, false);
                        }
                }

                ui_end();

                if (app.next_dir[0] != '\0')
                {
                        app_load_dir(&app, app.next_dir);
                        app.next_dir[0] = '\0';
                        first_frame = true;
                }
        }
        return 0;
}